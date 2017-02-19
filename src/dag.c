#include "dag.h"
#include <strings.h>
#include <unistd.h>

//#define DEBUG

// used by progress (but requires task->info point to an int)
#define minfo(x) ((x)->info == NULL ? 0 : *(int *)(x)->info)

#ifdef DEBUG
#define progress(...) printf(__VA_ARGS__)
#else
#define progress(...)
#endif

// Initial and max deque sizes for ea. thread.
#define INIT_STACK (128)
#define MAX_STACK (32768)

// Every task must maintain a TaskList of its current successors.
// (starts at 2 elems)
struct TaskList {
    turf_atomic16_t tasks;
    uint16_t avail; // max available space
    task_t *task[];
};

struct GlobalInfo {
    // For accessing other threads.
    thread_queue_t *threads;
    int nthreads;
    run_fn run;
    void *runinfo;

    struct TaskList *initial; // where tasks was copied to avail for convenience
};

// dag.h: typedef struct ThreadQueue thread_queue_t;
struct ThreadQueue {
    pthread_mutex_t L;
    task_t **deque;
    task_t **T, **H, **E;

    int deque_size; // len(deque)
    int rank;
    struct GlobalInfo *global;
#ifdef TIME_THREADS
    FILE *event_log;
#endif
};

#ifdef TIME_THREADS
#include <sys/time.h>
static time_t dag_start_time;
#define log_event(s) { \
    struct timeval tp; \
    gettimeofday(&tp, NULL); \
    fprintf(thr->event_log, "%ld.%06d: %s\n", \
            tp.tv_sec - dag_start_time, tp.tv_usec, s); \
}
#else
#define log_event(s)
#endif

/********************* Work queue routines *****************/
// Called by own thread when push() finds space is exhausted.
static void resize_deque(thread_queue_t *thr) {
    int sz;
    lock(&thr->L);
    sz = thr->T - thr->H;
    if(sz > MAX_STACK) {
        fprintf(stderr, "Thread %d has stack size %d (> max=%d)\n",
                         thr->rank, sz, MAX_STACK);
        exit(1);
    }
    if(sz > thr->deque_size/2) {
        task_t **deq = realloc(thr->deque, sz*2*sizeof(task_t *));
        if(deq == NULL) {
            fprintf(stderr, "Memory error: stack size = %d\n", sz);
            exit(1);
        }
        thr->T += deq - thr->deque; // relocate ptrs
        thr->H += deq - thr->deque;
        thr->E += deq - thr->deque;
        thr->deque = deq;
        thr->deque_size = sz*2;
    }
    if(thr->H != thr->deque) { // move back to start
        bcopy(thr->H, thr->deque, sz*sizeof(task_t *));
        thr->T -= thr->H - thr->deque;
        thr->E -= thr->H - thr->deque;
        thr->H = thr->deque;
    }
    unlock(&thr->L);
}

// THE protocol.
// push / pop from own queue
static void push(thread_queue_t *thr, task_t *task) {
    // Next push is out of bounds -- need to resize stack.
    if(thr->T - thr->deque >= thr->deque_size) {
        resize_deque(thr);
    }

    *thr->T = task;
    thr->T++;
}

static task_t *pop(thread_queue_t *thr) {
    thr->T--;
    //turf_threadFenceRelease(); // release change to T
    //turf_threadFenceAcquire(); // be sure E is there
    __asm__ volatile ("mfence":::"memory");
    if(thr->E > thr->T) { // handle exception
        thr->T++;
        lock(&thr->L);
        if(thr->E > thr->T) {
            progress("Detected error in pop()\n");
            unlock(&thr->L);
            return NULL;
        }
        thr->T--;
        if(thr->H > thr->T) {
            thr->T++;
            unlock(&thr->L);
            return NULL;
        }
        unlock(&thr->L);
    } else {
        progress("%d: successful pop (%d)\n", thr->rank, minfo(*thr->T));
    }
    return *thr->T;
}

// steal task from victim
static task_t *steal(thread_queue_t *v) {
    task_t *ret;
    lock(&v->L); // should acquire T
    v->E++;
    //turf_threadFenceRelease(); // release change to E
    //turf_threadFenceAcquire(); // be sure T is there
    if(v->E > v->T) {
        v->E--;
        unlock(&v->L);
        return NULL;
    }
    ret = *v->H;
    v->H++;
    unlock(&v->L);
    return ret;
}

static task_t *get_work(thread_queue_t *thr) {
    task_t *w = pop(thr);

    while(w == NULL) {
        if(thr->E > thr->T) {
            progress("Found error condition!\n");
            return NULL;
        }

        log_event("Attempting Steal");
        { // try to steal
            unsigned int k = random();
            unsigned int mod = thr->global->nthreads-1;
            for(unsigned int j=0; j<mod; j++) {
                int v = (j+k)%mod;
                v += v >= thr->rank;
                w = steal(&thr->global->threads[v]);
                if(w != NULL) {
                    progress("steal success %d <- %d (%d)\n", thr->rank, v,
                                                minfo(w));
                    log_event("Steal Success");
                    return w;
                }
            }
            usleep(random()%100);
            //usleep(random()%10);
        }
    }
    return w;
}

/******************** Per-task successor tracking routines **************/
// Works on a single tasklist,
//   returning 0 on success,
//          or 1 if resize is needed,
//          or 2 if resize is needed and this thread should do it.
static int add_task(struct TaskList *list, struct Task *s) {
    uint16_t i = turf_fetchAdd16Relaxed(&list->tasks, 1);
    if(i >= list->avail) {
        return 1 + (i == list->avail);
    }
    // incr before storing s
    turf_fetchAdd16(&s->joins, 1, TURF_MEMORY_ORDER_ACQUIRE);
    list->task[i] = s;
    return 0;
}

// Called when old->tasks == old->avail
// Speculatively create a larger list.
static struct TaskList *grow_tasklist(struct Task *n, struct TaskList *old) {
    int sz = old->avail*2; //old->avail < 4 ? 8 : old->avail*2;
    struct TaskList *list = calloc(sizeof(struct TaskList)
                                  + sz*sizeof(struct Task *), 1);
    for(int i=0; i<old->avail; i++) {
        if( (list->task[i] = old->task[i]) == NULL) {
            //free(list); // incomplete write
            //return turf_loadPtrRelaxed(&n->successors);

            // The writing thread promised they would fill this in...
            // we may be in an infinite loop otherwise.
            // To avoid waiting on a magical variable propagating here,
            i--;
            turf_threadFenceAcquire(); // need to get task[i]
            continue;
        }
    }
    turf_store16Relaxed(&list->tasks, old->avail);
    list->avail = sz;

    { // check to see if another thread has replaced the list meanwhile
        struct TaskList *prev = turf_compareExchangePtr(
                &n->successors, old, list, TURF_MEMORY_ORDER_RELEASE);
        if(prev == old) { // succeed.
            progress("New successor list creation succeeds.\n");
            free(old);
            return list;
        }
        progress("New successor list creation fails.\n");
        // fail.
        free(list);
        return prev;
    }
}

// Add successor `s` to task `n`.
//  returns 0 if `n` is already complete (no addition possible)
//  or 1 after addition.
static int add_successor(task_t *n, task_t *s) {
    struct TaskList *list = turf_loadPtrRelaxed(&n->successors);
    int r = 0;

    while(1) {
        if(list == NULL) return 0;
        if( (r = add_task(list, s))) { // need resize
            if(r == 1) { // first overflow
                list = grow_tasklist(n, list);
                continue;
            }
        } else {
            return 1; // It just happened.
        }

        list = turf_loadPtrRelaxed(&n->successors); // double-check.
    };

    return 0; // YDNBH;
}

static void enable_task(thread_queue_t *thr, task_t *n, task_t *s) {
    int16_t joins = turf_fetchAdd16Relaxed(&s->joins, -1) - 1;
    if(joins == 0) {
        push(thr, s); // Enqueue task.
    }
    progress("%d: %d --> %d (%d)\n", thr->rank, minfo(n), minfo(s), joins);
}

// Completed task 'n' -- block further additions and
// decrement join counter of all its successors.
// If the join counter reaches zero, add them to thr's deque.
//
// This returns the number of new additions.
// If this is zero, the caller deduces the DAG is complete.
//
// Notes:
//   Only one thread (the running thread) will ever call this routine,
//   and it only occurs once for each task.
//   It processes all the successors of the task.
static int enable_successors(thread_queue_t *thr, struct Task *n) {
    struct TaskList *list = turf_exchangePtr(&n->successors, NULL,
                                             TURF_MEMORY_ORDER_ACQUIRE);
    // Stop further writes to the list.
#ifdef DEBUG
    if(list == NULL) { // sanity check.
        printf("Error - NULL successors list for (%d)?\n", minfo(n));
        fflush(stdout);
        exit(1);
    }
#endif
    int16_t tasks = turf_fetchAdd16Relaxed(&list->tasks, list->avail+1);

    int16_t i;
    for(i=0; i<tasks; i++) {
        struct Task *s = list->task[i];
        if(s == NULL) {
            i--;
            turf_threadFenceAcquire(); // need to get task[i]
            continue;
        }
        enable_task(thr, n, s);
    }

    free(list);
    return tasks;
}

/***************** Per-Thread Programs *****************/
#define MIN(a,b) ((a) < (b) ? (a) : (b))
static void thread_ctor(int rank, int nthreads, void *data, void *info) {
    thread_queue_t *thr = (thread_queue_t *)data;

    Pthread_mutex_init(&thr->L, NULL);
    thr->deque = malloc(sizeof(struct Task *)*INIT_STACK);
    thr->T = thr->H = thr->E = thr->deque;

    thr->deque_size = INIT_STACK;
    thr->rank = rank;
    thr->global = (struct GlobalInfo *)info;
    if(rank == 0) {
        thr->global->threads = thr;
    }

    { // seed tasks.
        int u = -thr->rank;
        task_t tu = {.info = &u};

        int avail = thr->global->initial->avail;
        int mine  = avail/nthreads + (rank < (avail%nthreads));
        int start = (avail/nthreads)*rank + MIN(avail%nthreads, rank);
        progress("%d: Initial start = %d, mine = %d\n", thr->rank, start, mine);
        for(int i=0; i<mine; i++) {
            task_t *task = thr->global->initial->task[i+start];
            // Double-check here.
            enable_task(thr, &tu, task);
        }
    }
#ifdef TIME_THREADS
    { char log_name[] = "event-000.log";
      log_name[6] += (thr->rank/100)%10;
      log_name[7] += (thr->rank/10)%10;
      log_name[8] += (thr->rank/1)%10;
      thr->event_log = fopen(log_name, "a");
      if(thr->event_log == NULL) {
          exit(1);
      }
      log_event("=== Init Thread ===\n");
    }
#endif
}

static void thread_dtor(int rank, int nthreads, void *data, void *info) {
    thread_queue_t *thr = (thread_queue_t *)data;
    free(thr->deque);
    Pthread_mutex_destroy(&thr->L);
#ifdef TIME_THREADS
    fclose(thr->event_log);
#endif
}

static void *thread_work(void *data) {
    thread_queue_t *thr = (thread_queue_t *)data;
    task_t *task = NULL;

    // TODO: optimize handling of starvation and completion conditions.
    while( (task = get_work(thr))) {
        task_t *start = NULL;
        void *info = get_task_info(task);

        progress("%d: working on (%d)\n", thr->rank, minfo(task));
        if(info != NULL) {
            log_event("Running Task");
            start = thr->global->run(get_task_info(task),
                                             thr->global->runinfo);
            log_event("Adding Deps");
        }
        if(start == NULL) {
            if(enable_successors(thr, task) == 0) {
                goto final;
            }
        } else {
            if(enable_successors(thr, start) == 0) {
                goto final;
            }
            del_tasks(1, start);
        }
    }
    log_event("Received Term Signal");
    return NULL;
final: // found end, signal all dag-s
    progress("Thread %d: Signaling other threads.\n", thr->rank);
    log_event("Sending Term Signal");
    for(int k=0; k<thr->global->nthreads; k++) {
        if(k == thr->rank) continue;
        lock(&thr->global->threads[k].L);
        thr->global->threads[k].E += MAX_STACK;
        unlock(&thr->global->threads[k].L);
    }
    turf_threadFenceRelease();
    return NULL;
}

/*************** top-level API ****************/
void *get_task_info(task_t *task) {
    return turf_loadPtr(&task->info, TURF_MEMORY_ORDER_ACQUIRE);
}
void *set_task_info(task_t *task, void *info) {
    return turf_compareExchangePtr(&task->info, NULL, info,
                                   TURF_MEMORY_ORDER_RELEASE);
}

void del_tasks(int n, task_t *task) {
    for(int i=0; i<n; i++) {
        struct TaskList *list = turf_loadPtr(&task[i].successors,
                                             TURF_MEMORY_ORDER_ACQUIRE);
        if(list) free(list); // incomplete?
    }
    free(task);
}

static inline void init_task(task_t *task) {
    struct TaskList *list = calloc(sizeof(struct TaskList)
                                   + 2*sizeof(task_t *), 1);
    list->avail = 2;

    turf_storePtrRelaxed(&task->info, NULL);
    turf_storePtrRelaxed(&task->successors, list);
    turf_store16Relaxed(&task->joins, 0);
}
task_t *new_tasks(int n) {
    task_t *task = calloc(sizeof(task_t), n);
    for(int i=0; i<n; i++) {
        init_task(task+i);
    }
    return task;
}
task_t *start_task() {
    return new_tasks(1);
}

// Returns '1' if the dep is incomplete (and thus linking was
// successful) or else '0' if the dep is already complete.
int link_task(task_t *task, task_t *dep) {
    int n;
    return add_successor(dep, task);
}

void exec_dag(task_t *start, run_fn run, void *runinfo) {
    struct GlobalInfo global = {
        .threads = NULL, // filled in by rank 0
        .nthreads = 8,
        .run = run,
        .runinfo = runinfo,
        .initial = turf_exchangePtrRelaxed(&start->successors, NULL)
    };
    // TODO: Create global mutex/condition
    // for handling task starvation and determining whether
    // work is complete.
    del_tasks(1, start);

    global.initial->avail = turf_load16Relaxed(&global.initial->tasks);
    if(global.initial->avail < 1) {
        fprintf(stderr, "exec_dag: start task has no dependencies!\n");
        free(global.initial);
        return;
    }

#ifdef TIME_THREADS
    dag_start_time = time(NULL);
#endif
    run_threaded(global.nthreads, sizeof(thread_queue_t), &thread_ctor,
                 &thread_work, &thread_dtor, &global);
    free(global.initial);
}

