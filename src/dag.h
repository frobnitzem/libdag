#ifndef LIBDAG_C_H
#define LIBDAG_C_H

#include <stdatomic.h>

#include "dag_thread.h"

// Internal data structure wrapping each task in libdag.
typedef struct ThreadQueue thread_queue_t;
typedef struct Task task_t;

struct Task {
    _Atomic(void *) info; // actual user task info
    _Atomic(void *) successors; // List of dependencies
                                // (or NULL if node is complete).
    _Atomic(int) joins;
};

/* Callback types */
// Execute a single task (may add new nodes and signal with non-NULL return).
typedef task_t *(*run_fn)(void *a, void *runinfo);
// Run in serial before launching threads.  It returns runinfo for that thread.
typedef void *(*setup_fn)(int rank, int threads, void *info0);
// Run in serial after all threads are joined.
typedef void (*dtor_fn)(int rank, int threads, void *runinfo, void *info0);

task_t *new_tasks(int n);
task_t *start_task();
void del_tasks(int n, task_t *task);

// Returns 1 on success and 0 when linking was not needed.
// Join counters are automatically handled,
// so this is just informational and shouldn't be used practically.
int link_task(task_t *task, task_t *dep);
void exec_dag(task_t *start, run_fn run, void *runinfo);
// threads < MAX_THREAD
void exec_dag2(task_t *start, run_fn run, setup_fn init, dtor_fn dtor,
               int threads, void *info0);

void *get_task_info(task_t *task);
void *set_task_info(task_t *task, void *info);
int activate_task(task_t *task, void *info, task_t *start);

// default number of threads
#ifndef NTHREADS
#define NTHREADS 8
#endif

// Used to indicate error conditions as
// "available queue entries < -MAX_THREADS".
// This works because it's not possible to
// have more than MAX_THREADS simultaneous steal attempts,
// so available queue entries can't naturally go lower than that.
//
// Note: MAX_THREADS is not used to reserve static memory and can
// thus be arbitrarily large.  However, if you have more than
// 1000 threads, you must change the log naming code around
// "event-000.log" in dag.c or else event logging will not work.
#define MAX_THREADS (100000)

#endif
