#include "dag.h"
#include "turf/atomic.h"
#include <strings.h>
#include <unistd.h>

// Initial and max deque sizes for ea. thread.
#define INIT_STACK (128)
#define MAX_STACK (32768)

// Every task must maintain a TaskList of its current successors.
// (starts at 2 elems)
struct Task {
    void *info; // actual user task info
    turf_atomic16_t joins;
    turf_atomicPtr_t successors; // List of dependencies
                                 // (or NULL if node is complete).
};
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

struct ThreadQueue {
    pthread_mutex_t L;
    task_t **deque;
    task_t **T, **H, **E;

    pthread_t tid;
    int deque_size; // len(deque)
    int rank;
    struct GlobalInfo *global;
};

/********************* Work queue routines *****************/
// Called by main thread when push() finds space is exhausted.
static void resize_deque(thread_queue_t *thr) {
    int sz;
    lock(&thr->L);
    sz = thr->T - thr->H;
    if(sz > MAX_STACK) {
        fprintf(stderr, "Thread %d of stack: %d > %d\n",
                         thr->rank, sz, MAX_STACK);
        exit(1);
    }
    if(sz > thr->deque_size/2) {
        task_t **deq = realloc(thr->deque, sz*2);
        if(deq == NULL) {
            fprintf(stderr, "Memory error: stack size = %d\n", sz);
            exit(1);
        }
        thr->T += deq - thr->deque; // relocate ptrs
        thr->H += deq - thr->deque;
        thr->E += deq - thr->deque;
        thr->deque = deq;
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
    turf_threadFenceRelease();
    turf_threadFenceAcquire();
    if(thr->E > thr->T) { // handle exception
        thr->T++;
        lock(&thr->L);
        if(thr->E > thr->T) {
            printf("Detected error in pop()\n");
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
    }
    return *thr->T;
}

// thr is the victim
static task_t *steal(thread_queue_t *v) {
    lock(&v->L);
    v->E++;
    if(v->E > v->T) {
        v->E--;
        unlock(&v->L);
        return NULL;
    }
    v->H++;
    unlock(&v->L);
    return *v->H;
}

static task_t *get_work(thread_queue_t *thr) {
    task_t *w = pop(thr);

    while(w == NULL) {
        if(thr->E > thr->T) {
            printf("Found error condition!\n");
            return NULL;
        }

        { // try to steal
            unsigned int k = random();
            unsigned int mod = thr->global->nthreads-1;
            for(unsigned int j=0; j<mod; j++) {
                int v = (j+k)%mod;
                v += v >= thr->rank;
                w = steal(&thr->global->threads[v]);
                if(w != NULL)
                    break;
            }
            printf("Thread %d: no threads with available work.\n", thr->rank);
            //usleep(random()%100);
            sleep(1);
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
            free(old);
            return list;
        }
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
static int mv_successors_to_deque(thread_queue_t *thr, struct Task *n) {
    struct TaskList *list = turf_exchangePtr(&n->successors, NULL,
                                             TURF_MEMORY_ORDER_ACQUIRE);
    // Stop further writes to the list.
    int16_t tasks = turf_fetchAdd16Relaxed(&list->tasks, list->avail+1);

    int16_t i;
    for(i=0; i<tasks; i++) {
        struct Task *s = list->task[i];
        if(s == NULL) {
            i--;
            turf_threadFenceAcquire(); // need to get task[i]
            continue;
        }
        if(turf_fetchAdd16Relaxed(&s->joins, -1) == 1) {
            push(thr, s); // Enqueue task.
        }
    }

    free(list);
    return tasks;
}

/***************** Per-Thread Programs *****************/
#define MIN(a,b) (a) < (b) ? (a) : (b);
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
        int mine = thr->global->initial->avail;
        int start = mine;
        mine = mine/nthreads + (rank < (mine%nthreads));
        start = (start/nthreads)*rank + MIN(start%nthreads, rank);
        for(int i=0; i<mine; i++) {
            push(thr, thr->global->initial->task[i+start]);
        }
    }
}

static void thread_dtor(int rank, int nthreads, void *data, void *info) {
    thread_queue_t *thr = (thread_queue_t *)data;
    free(thr->deque);
    Pthread_mutex_destroy(&thr->L);
}

static void *thread_work(void *data) {
    thread_queue_t *thr = (thread_queue_t *)data;
    task_t *task = NULL;

    // TODO: optimize handling of starvation and completion conditions.
    while( (task = get_work(thr))) {
        task_t *start = thr->global->run(task->info, thr->global->runinfo);
        if(start == NULL) {
            if(mv_successors_to_deque(thr, task) == 0) {
                goto final;
            }
        } else {
            if(mv_successors_to_deque(thr, start) == 0) {
                goto final;
            }
            del_task(start);
        }
    }
    return NULL;
final: // found end, signal all dag-s
    printf("Thread %d: Signaling other threads.\n", thr->rank);
    for(int k=0; k<thr->global->nthreads; k++) {
        k += k == thr->rank;
        thr->global->threads[k].E += MAX_STACK;
    }
    turf_threadFenceRelease();
    return NULL;
}

void del_task(task_t *task) {
    struct TaskList *list = turf_loadPtr(&task->successors,
                                         TURF_MEMORY_ORDER_ACQUIRE);
    if(task) free(list); // incomplete?
    free(task);
}

/*************** top-level API ****************/
// OK to send NULL for start before running.
// During execution, start should always be non-NULL
// to prevent premature execution.
task_t *new_task(void *a, task_t *start) {
    task_t *task = malloc(sizeof(task_t));
    struct TaskList *list = calloc(sizeof(struct TaskList)
                                   + 2*sizeof(task_t *), 1);
    list->avail = 2;

    task->info = a;
    turf_storePtrRelaxed(&task->successors, list);

    if(start != NULL) {
        link_task(task, start);
    }
    return task;
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
    del_task(start);

    global.initial->avail = turf_load16Relaxed(&global.initial->tasks);
    if(global.initial->avail < 1) {
        fprintf(stderr, "exec_dag: start task has no dependencies!\n");
        free(global.initial);
        return;
    }

    run_threaded(global.nthreads, sizeof(thread_queue_t), &thread_ctor,
                 &thread_work, &thread_dtor, &global);
    free(global.initial);
}

