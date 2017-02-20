#include <dag.h>
#include <math.h>
#include <stdint.h>
#include "test.h"

#ifndef M_PI
#define M_PI (3.141592653589793)
#endif

// test 1
void setup(int rank, int tasks, void *data, void *info) {
    printf("# setup rank %d/%d\n", rank, tasks);
}
void *work(void *data) {
    printf("# run\n");
    return NULL;
}
void dtor(int rank, int tasks, void *data, void *info) {
    printf("# cleanup rank %d/%d\n", rank, tasks);
}

// test 2-3
typedef struct {
    int data, ok;
    _Atomic(int) atom;
} global_info_t;
typedef struct {
    int rank, tasks;
    _Atomic(int) *atom;
    int *data;
    int *ok;
} thread_info_t;

void setup2(int rank, int tasks, void *data, void *info) {
    global_info_t *g = (global_info_t *)info;
    thread_info_t *thr = (thread_info_t *)data;

    printf("# setup rank %d/%d\n", rank, tasks);
    thr->rank = rank;
    thr->tasks = tasks;
    thr->atom = &g->atom;
    thr->data = &g->data;
    thr->ok = &g->ok;
}
void *work2(void *data) {
    thread_info_t *thr = (thread_info_t *)data;
    printf("# run rank %d/%d\n", thr->rank, thr->tasks);
    if(thr->rank == 1) { // producer
        *thr->data = 31337;
        atomic_store_explicit(thr->atom, 42, memory_order_release);
    } else { // consumer
        int i;
        while( !(i = atomic_load_explicit(thr->atom, memory_order_acquire)) );
        *thr->ok = (i == 42 && *thr->data == 31337);
    }
    return NULL;
}
void dtor2(int rank, int tasks, void *data, void *info) {
    printf("# cleanup rank %d/%d\n", rank, tasks);
}

// remaining tests
// from code.google.com/p/smhasher/wiki/MurmurHash3
inline static uint32_t integerHash(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h & 0xffff; // modification to make 16-bit
}

typedef struct Oper oper_t;
struct Oper {
    int id, val;
    oper_t *a, *b;
};
task_t *hash_two(void *x, void *runinfo) {
    oper_t *op = (oper_t *)x;
    if(op->a == NULL) { // leaf node
        goto end;
    }
    if(op->b == NULL) { // single hash
        op->val = integerHash(op->a->val);
        goto end;
    }
    op->val = integerHash(op->a->val | (op->b->val<<16));
end:
    if(op->id < 100) // Don't go overboard with the printing.
        printf("# node %d ~> %04x\n", op->id, op->val);
    return NULL;
}

// test 4
int single_dag() {
    task_t *start = start_task();
    oper_t op = {0, 1, NULL, NULL};
    task_t *end = new_tasks(1);
    link_task(end, start);
    set_task_info(end, &op);

    exec_dag(start, hash_two, NULL);
    del_tasks(1, end);

    return 0;
}

// test 5
int check10(oper_t *dag) {
    uint16_t ref[10] = {
        1, 2, 0x0e8b, 4, 0xc306, 0xeca8, 0x2b94, 0x9f28, 0x66a3, 0xcdd8
    };
    for(int i=0; i<10; i++) {
        if(dag[i].val != ref[i]) {
            printf("# mismatch at node %d (%04x vs %04x)\n",
                        i, dag[i].val, ref[i]);
            return 1;
        }
    }
    return 0;
}
void setup10(oper_t dag[10]) {
    for(int i=0; i<10; i++) {
        dag[i].id = i;
        dag[i].val = i+1;
        dag[i].a = dag[i].b = NULL;
    }
    dag[2].a = &dag[0];
    dag[2].b = &dag[1];
    dag[4].a = &dag[1];

    dag[5].a = &dag[2];
    dag[5].b = &dag[3];
    dag[6].a = &dag[3];
    dag[6].b = &dag[4];

    dag[7].a = &dag[5];
    dag[7].b = &dag[6];
    dag[8].a = &dag[6];

    dag[9].a = &dag[7];
    dag[9].b = &dag[8];
}
/* void clear_dag(oper_t *dag) {
    for(int i=0; i<10; i++) {
        dag[i].val = i+1;
    }
}*/
    
int dag10() {
    oper_t dag[10];
    task_t *task = new_tasks(10);
    setup10(dag);
    task_t *start = start_task();

    // create task_t-s from oper_t-s
    for(int i=0; i<10; i++) {
        set_task_info(task+i, &dag[i]);

        if(dag[i].a == NULL) { // no deps
            link_task(task+i, start);
        } else { // Since we know op-s are in topo-sort order,
            int j = dag+i - dag[i].a; // dag[i] > dag[i].a,b.
            link_task(&task[i], &task[i-j]);
            if(dag[i].b != NULL) {
                int j = dag+i - dag[i].b;
                link_task(&task[i], &task[i-j]);
            }
        }
    }

    exec_dag(start, hash_two, NULL);
    del_tasks(10, task);

    return check10(dag);
}

// test 6
task_t *noop(void *a, void *b) {
    return NULL;
};
int wide_dag() {
    int ret = 1, j = 0, width = 20000;
    int *ids = malloc(sizeof(int)*width);
    task_t *start = start_task();
    task_t *end = new_tasks(1);
    task_t *tasks = new_tasks(width);
    if(end == NULL || tasks == NULL) {
        printf("# memory alloc error.\n");
        return 1;
    }
    for(int i=0; i<width; i++) {
        ids[i] = i+1;
        link_task(tasks+i, start);
        set_task_info(tasks+i, &ids[i]);
        j += link_task(end, tasks+i);
    }
    printf("# linked %d tasks\n", j);
    if(j != width) {
        goto err;
    }

    exec_dag(start, noop, NULL);
    ret = 0;

err:
    del_tasks(width, tasks);
    del_tasks(1, end);
    free(ids);

    return ret;
}

// test 7
typedef struct FFTNode fft_node_t;
struct FFTNode {
    int id;
    fft_node_t *l, *r;
    double re, im;
};

task_t *first_task;
int levels;
double *inp;

// number of sub-tasks at level k (grouped together in blocks of mem).
#define SUBTASKS(k) ((k) == 0 ? 1 : (((k)+1)*(1<<(k))))
// Create subtask at level k
// 1<<k nodes are added first to the buffer, and remaining k-1 levels
// are added at offsets of skipL and skipR
// Obviously, node and task must have size
// SUBTASKS(k) = 1<<k + 2*SUBTASKS(k-1).
//
// Note: Setup is likely more expensive than actually running the DAG...
void mk_butterfly(fft_node_t *node, task_t *task, task_t *start, int k,
                    unsigned addr) {
    if(k == 0) {
        // node->re, im contain the input data.
        link_task(task, start);
        set_task_info(task, node);
        node->l = NULL; // We could start at k=1, but I'm lazy.
        //printf("task[%d] : 0,0 = %p\n", (task-first_task), task);
        node->re = inp[addr];
        node->im = 0.0;
        node->id = task-first_task;
        //printf("Node %d: %d = %f\n", node->id, addr, inp[addr]);
        return;
    }
    int n = 1<<k;
    int nsub = SUBTASKS(k-1);
    int skipL = n;
    int skipR = n+nsub;
    //printf("n,nsub,skipL,skipR = %d,%d,%d,%d\n", n, nsub, skipL, skipR);

    mk_butterfly(node+skipL, task+skipL, start, k-1, addr);
    mk_butterfly(node+skipR, task+skipR, start, k-1, addr | (1<<(levels-k)));
    for(int i=0; i<n; i++) {
        int off = i < n/2 ? i : i-n/2;
        node[i].im = (double)i/n; // which root to use here.
        node[i].l = node+skipL+off;
        node[i].r = node+skipR+off;
        node[i].id = task-first_task + i;
        //printf("Node %d: %d,%d = %f\n", node[i].id, k, i, node[i].im);

        set_task_info(task+i, node+i);
        link_task(task+i, &task[skipL+off]);
        // TODO: Sending the wrong addresses to link_task
        // is painful to debug.  I think the hash-map for node id-s is
        // the way to go.
        //printf("task[%d] : %d,%d = %p\n", task-first_task+i, k, i, task+i);
        link_task(task+i, &task[skipR+off]);
    }
}
task_t *fft_step(void *info, void *global) {
    fft_node_t *n = (fft_node_t *)info;
    if(n == NULL || n->l == NULL) return NULL;

    //printf("# %d: %f\n", n->id, n->im);
    double c = cos(2.0*M_PI*n->im);
    double s = sin(2.0*M_PI*n->im);
    n->re = n->l->re + c*n->r->re - s*n->r->im;
    n->im = n->l->im + c*n->r->im + s*n->r->re;
    return NULL;
}
int butterfly() {
    int err = 0;
    int n = 1<<levels;
    int n2 = 1<<(levels-1);

    inp = calloc(n, sizeof(double));
    fft_node_t *node = malloc(sizeof(fft_node_t)*SUBTASKS(levels));
    task_t *task = new_tasks(SUBTASKS(levels));
    task_t *start = start_task(NULL, NULL);
    task_t *end = new_tasks(1);
    if(inp == NULL || task == NULL || node == NULL || end == NULL) {
        perror("# memory error");
        return 1;
    }

    for(int i=1; i<n; i += 2) {
        inp[i] = 1.0;
    }

    first_task = task;
    mk_butterfly(node, task, start, levels, 0);
    for(int i=0; i<n; i++) {
        link_task(end, task+i);
    }

    exec_dag(start, fft_step, NULL);
    // test result
    if(fabs(node[0].re - n2) > 1e-8) {
        printf("# FFT 0-channel %f != %d\n", node[0].re, n);
        err++;
    }
    if(fabs(node[n2].re + n2) > 1e-8) {
        printf("# FFT Nyquist-channel %f != -%d\n", node[n2].re, n2);
        err++;
    }
    for(int i=1; i<n; i++) {
        if(i == n2) continue;
        if(fabs(node[i].re) > 1e-8) {
            printf("# FFT channel re[%d]: %f != 0.0\n", i, node[i].re);
            err++;
        }
    }
    for(int i=0; i<n; i++) {
        if(fabs(node[i].im) > 1e-8) {
            printf("# FFT channel im[%d]: %f != 0.0\n", i, node[i].im);
            err++;
        }
    }

    n = SUBTASKS(levels);
    del_tasks(1, end);
    del_tasks(n, task);
    free(node);
    free(inp);

    return err;
}

int main(int argc, char *argv[]) {
    global_info_t g = { .data = 0 };
    atomic_store_explicit(&g.atom, 0, memory_order_relaxed);

    printf("1..9\n");
    check("Simple threaded run succeeds",
          run_threaded(4, 0, &setup, &work, &dtor, NULL));
    check("REL/ACQ threaded run succeeds",
          run_threaded(2, sizeof(thread_info_t), &setup2, &work2, &dtor2, &g));
    check("Producer / consumer memory fence worked", !g.ok);

    check("single-node dag execution succeeds", single_dag());

    check("10-node dag has correct execution order", dag10());

    check("wide parallel dag succeeds", wide_dag());

    levels = 2;
    check("butterfly-2 dag succeeds", butterfly());
    levels = 6;
    check("butterfly-6 dag succeeds", butterfly());
    levels = 10;
    check("butterfly-10 dag succeeds", butterfly());
}

