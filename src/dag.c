#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "dag.h"

// responds to options: DEBUG, TIME_THREADS, NTHREADS

#ifndef NTHREADS
#define NTHREADS 8
#endif

// used by progress (but requires task->info point to an int)
#define minfo(x) ((x)->info == NULL ? 0 : *(int *)(x)->info)

// Used to determine error conditions
// (when available queue entries < -MAX_THREADS).
// Of course, it's not possible to naturally
// have more the > MAX_THREADS simultaneous steal attempts
// required to detect a false error.
// Note: You must also change log naming code around "event-000.log"
// if MAX_THREADS will really be over 1000.
#define MAX_THREADS (1000)

#if NTHREADS > MAX_THREADS
#error "Can't have NTHREADS > MAX_THREADS"
#endif


#ifdef DEBUG
#define progress(...) printf(__VA_ARGS__)
#else
#define progress(...)
#endif

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


// Initial and max deque sizes for ea. thread.
// INIT_STACK must be a power of 2
#define INIT_STACK (128)
#define MAX_STACK (32768)

#define ERROR_COND (-MAX_THREADS-MAX_STACK)

// Every task must maintain a TaskList of its current successors.
// (starts at 2 elems)
struct TaskList {
    _Atomic(int) tasks;
    int avail; // max available space
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
    task_t **deque;
    unsigned T;
    _Atomic(unsigned ) H;
    _Atomic(int) nque;

    int deque_size; // len(deque)
    int rank;
    struct GlobalInfo *global;
#ifdef TIME_THREADS
    FILE *event_log;
#endif
};
#define DEQUE(n) thr->deque[(n) & (thr->deque_size-1)]

/********************* Work queue routines *****************/
// Called by own thread when push() finds space is exhausted.
static void resize_deque(thread_queue_t *thr) {
    // Set que size to zero, and store current size.
    // This halts any steal attempts (and therefore prevents access to H).
    int nque = atomic_exchange_explicit(&thr->nque, 0, memory_order_acquire);

    // Expected H after processing all pending steals.
    unsigned H = thr->T - nque;
    unsigned T = thr->T & (thr->deque_size-1);

    // Spin until pending steals are complete.
#ifdef DEBUG
    int H2;
    while( (H2 = atomic_load_explicit(&thr->H, memory_order_relaxed)) != H) {
        progress("%d: waiting for %d pending steal(s)\n", thr->rank, H-H2);
    }
#else
    while(atomic_load_explicit(&thr->H, memory_order_relaxed) != H);
#endif
    H = H & (thr->deque_size-1);

    if(thr->deque_size >= MAX_STACK) {
        fprintf(stderr, "Thread %d has stack size %d,"
                        " and will exceed max on resize.\n", thr->rank, nque);
        exit(1);
    }

    // Double the deque size.
    task_t **deq = realloc(thr->deque, thr->deque_size*2*sizeof(task_t *));
    if(deq == NULL) {
        fprintf(stderr, "Memory error: stack size = %d\n", thr->deque_size);
        exit(1);
    }
    if(T <= H) { // wrapped range
        // Check whether [0,T) or [H,deque_size) is a smaller copy size.
        if(T < thr->deque_size-H) {
            //progress("%d: resizing deque (left copy)\n", thr->rank);
            if(T > 0) {
                memcpy(deq+thr->deque_size, deq, T*sizeof(task_t *));
            }
            T += thr->deque_size;
        } else {
            //progress("%d: resizing deque (right copy)\n", thr->rank);
            memcpy(deq+thr->deque_size+H, deq+H,
                            (thr->deque_size-H)*sizeof(task_t *));
            H += thr->deque_size;
        }
    } else {
        //progress("%d: resizing deque (no copy)\n", thr->rank);
    }
    thr->deque = deq;
    thr->deque_size *= 2;

    atomic_store_explicit(&thr->H, H, memory_order_relaxed);
    thr->T = H + nque; // maintain T-H = nque

    // Add back the removed que elements to re-enable stealing.
    atomic_fetch_add_explicit(&thr->nque, nque, memory_order_release);
}

// Atomic sized dequeue protocol (replace THE protocol).
// push / pop from own queue
static void push(thread_queue_t *thr, task_t *task) {
    // 'n' is an upper bound on the que size, so tells us if a resize is needed.
    int n = atomic_load_explicit(&thr->nque, memory_order_relaxed);
    if(n >= thr->deque_size) {
        resize_deque(thr);
    }
    DEQUE(thr->T) = task;
    thr->T++;
    atomic_fetch_add_explicit(&thr->nque, 1, memory_order_release);
}

// returns 2 on error condition
//         1 on regular deque
//         0 on empty que
static int pop(thread_queue_t *thr, task_t *restrict *ret) {
    int n = atomic_fetch_add_explicit(&thr->nque, -1, memory_order_relaxed);
    if(n < 1) { // handle exception
        atomic_fetch_add_explicit(&thr->nque, 1, memory_order_relaxed);
        *ret = NULL;
        return 2*(n <= MAX_THREADS);
    }
    thr->T--;
    *ret = DEQUE(thr->T);
    progress("%d: successful pop (%d)\n", thr->rank, minfo(*ret));
    return 1;
}

// steal task from victim (thr)
static task_t *steal(thread_queue_t *thr) {
    int H, n = atomic_fetch_add_explicit(&thr->nque, -1, memory_order_acquire);
    if(n < 1) {
        atomic_fetch_add_explicit(&thr->nque, 1, memory_order_relaxed);
        return NULL;
    }
    H = atomic_fetch_add_explicit(&thr->H, 1, memory_order_relaxed);
    return DEQUE(H);
}

static task_t *get_work(thread_queue_t *thr) {
    task_t *w = NULL;

    while(w == NULL) {
        switch(pop(thr, &w)) {
            case 1:
                return w;
            case 0:
                break;
            case 2:
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
static int add_task(struct TaskList *list, task_t *s) {
    int i = atomic_fetch_add_explicit(&list->tasks, 1, memory_order_relaxed);
    if(i >= list->avail) {
        return 1 + (i == list->avail);
    }
    // incr before storing s
    atomic_fetch_add_explicit(&s->joins, 1, memory_order_acquire);
    list->task[i] = s;
    return 0;
}

// Called by the first adding thread what finds old->tasks == old->avail.
// Speculatively create a larger list.
// This is speculative because enable_successors may make the addition moot.
static struct TaskList *grow_tasklist(task_t *n, struct TaskList *old) {
    int sz = old->avail*2; //old->avail < 4 ? 8 : old->avail*2;
    struct TaskList *list = calloc(sizeof(struct TaskList)
                                  + sz*sizeof(task_t *), 1);
    for(int i=0; i<old->avail; i++) {
        if( (list->task[i] = old->task[i]) == NULL) {
            //free(list); // incomplete write
            //return atomic_load_explicit(&n->successors, memory_order_relaxed);

            // The writing thread promised they would fill this in...
            // we may be in an infinite loop otherwise.
            // To avoid waiting on a magical variable propagating here,
            i--;
            // need to get task[i]
            atomic_thread_fence(memory_order_acquire);
            continue;
        }
    }
    atomic_store_explicit(&list->tasks, old->avail, memory_order_relaxed);
    list->avail = sz;

    { // check to see if another thread has replaced the list meanwhile
        struct TaskList *prev = old;
        if(atomic_compare_exchange_strong_explicit(
                        &n->successors, &prev, list, memory_order_release,
                                                     memory_order_relaxed)) {
            // succeed.
            //progress("New successor list creation succeeds.\n");
            free(old);
            return list;
        }
        //progress("New successor list creation fails.\n");
        // fail.
        free(list);
        return prev;
    }
}

// Add successor `s` to task `n`.
//  returns 0 if `n` is already complete (no addition possible)
//  or 1 after addition.
static int add_successor(task_t *n, task_t *s) {
    struct TaskList *list = atomic_load_explicit(&n->successors, memory_order_relaxed);
    int r = 0;

    while(1) {
        if(list == NULL) return 0;
        if( (r = add_task(list, s))) { // need resize
            if(r == 2) { // first overflow
                list = grow_tasklist(n, list);
                continue;
            }
        } else {
            return 1; // It just happened.
        }

        // Wait until resize is complete.
        { struct TaskList *old = list;
          while( (list = atomic_load_explicit(&n->successors,
                          memory_order_relaxed)) == old);
        }
    };

    return 0; // YDNBH;
}

static void enable_task(thread_queue_t *thr, task_t *n, task_t *s) {
    int joins = atomic_fetch_add_explicit(&s->joins, -1, memory_order_relaxed)-1;
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
static int enable_successors(thread_queue_t *thr, task_t *n) {
    struct TaskList *list = atomic_exchange_explicit(&n->successors, NULL,
                                                     memory_order_acquire);
    // Stop further writes to the list.
#ifdef DEBUG
    if(list == NULL) { // sanity check.
        printf("Error - NULL successors list for (%d)?\n", minfo(n));
        fflush(stdout);
        exit(1);
    }
#endif
    int tasks = atomic_fetch_add_explicit(&list->tasks, list->avail+1,
                                              memory_order_relaxed);
    if(tasks > list->avail) // current list is full (mid-expansion)
        tasks = list->avail;

    int i;
    for(i=0; i<tasks; i++) {
        task_t *s = list->task[i];
        if(s == NULL) {
            i--;
            atomic_thread_fence(memory_order_acquire);
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

    thr->deque = malloc(sizeof(task_t *)*INIT_STACK);
    thr->T = 0;
    atomic_store_explicit(&thr->H, 0, memory_order_relaxed);
    atomic_store_explicit(&thr->nque, 0, memory_order_relaxed);

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
        //progress("%d: initial task range = [%d, %d)\n", thr->rank,
        //                start, start+mine);
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
        atomic_store_explicit(&thr->global->threads[k].nque, ERROR_COND, 
                              memory_order_relaxed);
    }
    atomic_thread_fence(memory_order_release);
    return NULL;
}

/*************** top-level API ****************/
void *get_task_info(task_t *task) {
    return atomic_load_explicit(&task->info, memory_order_acquire);
}

void *set_task_info(task_t *task, void *info) {
    void *out = NULL;
    atomic_compare_exchange_strong_explicit(&task->info, &out, info,
                              memory_order_release, memory_order_relaxed);
    return out;
}

void del_tasks(int n, task_t *task) {
    for(int i=0; i<n; i++) {
        struct TaskList *list = atomic_load_explicit(&task[i].successors,
                                                     memory_order_acquire);
        if(list) free(list); // This will be NULL unless the task was never completed.
    }
    free(task);
}

static inline void init_task(task_t *task) {
    struct TaskList *list = calloc(sizeof(struct TaskList)
                                   + 2*sizeof(task_t *), 1);
    list->avail = 2;

    atomic_store_explicit(&task->info, NULL, memory_order_relaxed);
    atomic_store_explicit(&task->successors, list, memory_order_relaxed);
    atomic_store_explicit(&task->joins, 0, memory_order_relaxed);
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
    return add_successor(dep, task);
}

void exec_dag(task_t *start, run_fn run, void *runinfo) {
    struct GlobalInfo global = {
        .threads = NULL, // filled in by rank 0
        .nthreads = NTHREADS,
        .run = run,
        .runinfo = runinfo,
        .initial = atomic_exchange_explicit(&start->successors, NULL, memory_order_relaxed)
    };
    // TODO: Create global mutex/condition
    // for handling task starvation and determining whether
    // work is complete.
    del_tasks(1, start);

    global.initial->avail = atomic_load_explicit(&global.initial->tasks, memory_order_relaxed);
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

