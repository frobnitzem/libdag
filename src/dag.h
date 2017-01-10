#include "dag_thread.h"

// Internal data structure wrapping each task in libdag.
typedef struct ThreadQueue thread_queue_t;
typedef struct Task task_t;

// Execute a single task (may add new nodes).
typedef task_t *(*run_fn)(void *a, void *runinfo);

task_t *new_task(void *a, task_t *start);
void del_task(task_t *task);

// Returns 1 on success and 0 when linking was not needed.
// Join counters are automatically handled,
// so this is just informational and shouldn't be used practically.
int link_task(task_t *task, task_t *dep);
void exec_dag(task_t *start, run_fn run, void *runinfo);
void *get_info(task_t *task);

