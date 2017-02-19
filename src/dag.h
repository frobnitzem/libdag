#include "dag_thread.h"
#include "turf/atomic.h"

// Internal data structure wrapping each task in libdag.
typedef struct ThreadQueue thread_queue_t;
typedef struct Task task_t;

// dag.h: typedef struct Task task_t;
struct Task {
    turf_atomicPtr_t info; // actual user task info
    turf_atomic16_t joins;
    turf_atomicPtr_t successors; // List of dependencies
                                 // (or NULL if node is complete).
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

