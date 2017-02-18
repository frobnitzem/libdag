libdag - a high-level C interface for parallel computation
of directed acyclic graphs (DAG)s

To suit the manifold different structures and constraints of parallel programs,
there exist an amazing diversity of good toolkits for parallel programming,
including thread building blocks (TBB), Cilkplus, MPI, etc. etc.
This library focuses on implementing computations on directed acyclic graphs,
where child nodes must be computed before parents but no other constraints are
present.
The implementation differs from others by allowing considerable
flexibility in how the DAG manages its data so the library itself
can provide only what is necessary to ensure correct execution order
-- children (aka dependencies) before parents (aka successors).


Using the Library
=================

There are 2 levels of helper functions included.  The simplest
is a set of thread helper functions, including especially
`run_threaded`, which takes per-thread setup, work, and completion
functions as arguments and blindly executes them.
To use it, just include `dag_thread.h`.

The next level, in `dag.h`, includes `dag_thread.h`, and
adds task execution scheduling.  It requires the user to build
a task graph (by wrapping the user's data structures inside
`task_t` objects, and then call `exec_dag`.  Other than the task API:

```
  get_info  : task_t -> A
  new_task  : A, Maybe task_t -> task_t  [ always creates a fresh task ]
  link_task : task_t -> task_t -> Int  [First arg is parent,
                                       second arg is child.]
  exec_dag  : task_t -> (A -> G -> task_t) -> G -> ()
  del_task  : task_t -> ()
```

the `task_t` structures are opaque.  The user must therefore use their own
data structure (type `A`) to hold the reference structure and
implement return value passing.

Task Execution
--------------

Using the library is really just as simple as calling
`new_task` and `link_task` while traversing the user's
data structure.

The caller must ensure that added
links do not create a cycle in the graph,
or else execution will deadlock.
A cycle happens whenever the added dependency is
reachable by traversing the the successors of the current node.
libdag does not check this, because it assumes the user
will actually add a DAG.

All DAGs must begin at a single `start` task and terminate
at a single final task.
The `start` task is a special (no-op) task
you create with <code>task_t *start = new_task(NULL, NULL)</code>.
Whenever a leaf task (i.e. a task with no dependencies)
is encountered, it must be linked as
the parent of the start task in order to be executed.
The final task is executed.  If your graph produces multiple
outputs, you'll need to make a special `end` task and link them
as children (for detecting algorithm termination).

For convenience, the second argument of `new_task`
is added as a dependency of the current task during initialization.
Before calling `exec_dag`, the second argument of `new_task`
is never really needed.  However, it can save the call to
`link_task(self, start)` that is required for all leaf nodes.

To run the DAG, pass the start task to `exec_dag`.
Note that the start task itself is immediately
free-ed by `exec_dag` and never executed.

Examples are available in the test directory, which
includes:

* simple use of `run_threaded`
* execution of a 10-node graph
* execution of 20,000 trivially parallel nodes
* computation of FFTs of size 2^k

Advanced Topics
===============

Expanding the task graph
------------------------

The DAG of tasks can be expanded during the task's compute
phase by calling `link_task` to add new dependencies
to the current node.
The code allows any new or existing
tasks to be added as dependencies.
The caller must still ensure that no cycles
are created.

Usually the compute task returns NULL.
To expand the task graph, the compute phase
instead returns a newly created
`start` task to indicate the additional tasks.
Since parallel execution is in progress
while expanding the task graph, it is important that
tasks created here supply this `start` task as the second arg of
`new_task`.  This will prevent the new tasks from
launching prematurely.

Also, the caller must arrange the currently running task
to be a successor of the newly added tasks
(reachable by some path from the new start).
If this is not done, then the computation
will deadlock and never complete.
Adding extra dependencies to the `start` task is OK.

A minimal example that just re-runs the current node would
therefore be:

    task_t *run(void *self, void *runinfo) {
        //... do some computation on self ...
        if(I_should_redo(self)) { 
     	    task_t *start = new_task(NULL, NULL);
            link_task(task_of(self), start);
            return start; // causes immediate enqueue of self
        }	
        return NULL; // no expansion
    }


DAG Coarsening
--------------

Good parallelism requires maximizing the time spent doing work
while minimizing time spent in the queueing algorithm.
If your parallelism is too fine-grained, it makes sense to coarsen
the DAG by combining subgraphs.  We have not yet implemented
library features to help with this process.

At a minimum, what is needed is:

1. A data structure, `B`, to hold subgraphs.
2. A function which takes a DAG of `A` type tasks
   and returns a list of `B` type tasks.
   - Ideally, this function should make use of an auxilliary
     function that estimates the work for each `A`-type task.

Finding Strongly Connected Components
-------------------------------------

If your task graph may contain cycles (e.g. co-recursive calls),
then you must first turn it into a DAG before using this library.
A helpful implementation of Tarjan's algo for
enumerating strongly connected components is present in `scc.c`.
