#include <dag.h>
#include "test.h"
#include <turf/atomic.h>

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
    turf_atomic16_t atom;
} global_info_t;
typedef struct {
    int rank, tasks;
    turf_atomic16_t *atom;
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
        turf_store16(thr->atom, 42, TURF_MEMORY_ORDER_RELEASE);
    } else { // consumer
        uint16_t i;
        while( !(i = turf_load16(thr->atom, TURF_MEMORY_ORDER_ACQUIRE)) );
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
    //printf("node %d ~> %04x\n", op->id, op->val);
    return NULL;
}

// test 4
int single_dag() {
    task_t *start = new_task(NULL, NULL);
    oper_t op = {0, 1, NULL, NULL};
    task_t *end = new_task(&op, start);

    exec_dag(start, hash_two, NULL);
    del_task(end);

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
        dag[i].id = 0;
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
    task_t *task[10];
    setup10(dag);
    task_t *start = new_task(NULL, NULL);

    // create task_t-s from oper_t-s
    for(int i=0; i<10; i++) {
        if(dag[i].a == NULL) { // no deps
            task[i] = new_task(NULL, start);
        } else { // Since we know op-s are in topo-sort order,
            int j = dag+i - dag[i].a; // dag[i] > dag[i].a,b.
            task[i] = new_task(NULL, NULL);
            link_task(task[i], task[i-j]);
            if(dag[i].b != NULL) {
                int j = dag+i - dag[i].b;
                link_task(task[i], task[i-j]);
            }
        }
    }

    exec_dag(start, hash_two, NULL);
    for(int i=0; i<10; i++) {
        del_task(task[i]);
    }

    return check10(dag);
}

int main(int argc, char *argv[]) {
    global_info_t g = { .data = 0 };
    turf_store16Relaxed(&g.atom, 0);

    printf("1..5\n");
    check("Simple threaded run succeeds",
          run_threaded(4, 0, &setup, &work, &dtor, NULL));
    check("REL/ACQ threaded run succeeds",
          run_threaded(2, sizeof(thread_info_t), &setup2, &work2, &dtor2, &g));
    check("Producer / consumer memory fence worked", !g.ok);

    check("single-node dag execution succeeds", single_dag());

    check("10-node dag has correct execution order", dag10());
}
