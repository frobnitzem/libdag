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

// Execute a single task (may add new nodes).
typedef task_t *(*run_fn)(void *a, void *runinfo);

task_t *new_tasks(int n);
task_t *start_task();
void del_tasks(int n, task_t *task);

// Returns 1 on success and 0 when linking was not needed.
// Join counters are automatically handled,
// so this is just informational and shouldn't be used practically.
int link_task(task_t *task, task_t *dep);
void exec_dag(task_t *start, run_fn run, void *runinfo);
void *get_task_info(task_t *task);
void *set_task_info(task_t *task, void *info);
int activate_task(task_t *task, void *info, task_t *start);

#endif
