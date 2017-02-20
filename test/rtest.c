#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include <pthread.h>

#include <dag.h>
#include <time.h>
#include "test.h"

#define USLEEP 100

// from code.google.com/p/smhasher/wiki/MurmurHash3
inline static uint32_t integerHash(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h & 0xffff; // modification to make 16-bit
}

#define MAX_CHILD 8

typedef struct NodeS node_t;
struct NodeS {
    int id;
    int nchild, nparent;
    int child[MAX_CHILD];
};

static const int MAXRANK = 500;

/* Pre-ordain width of each rank. */
int fill_widths(int wid[MAXRANK], int ranks, int min_wid, int max_wid) {
    int nodes = 0;
    for(int i = 0; i < ranks; i++) {
        wid[i] = min_wid + random() % (max_wid - min_wid + 1);
        nodes += wid[i];
    }
    return nodes;
}

node_t *gen_dag(int nodes, int ranks, int pct_edges, int pct_lazy,
                int wid[MAXRANK]) {
    node_t *node;

    if( (node = malloc(sizeof(node_t)*nodes)) == NULL) {
        return NULL;
    }
    for(int i=0; i<nodes; i++) { // initialize nodes.
        node[i].id = i+1;
        node[i].nchild = node[i].nparent = 0;
        if(random() % 100 < pct_lazy) {
            node[i].id = -(i+1);
        }
    }
    nodes = 0;

    for(int i = 0; i < ranks; i++) {
        int new_nodes = wid[i];

        for(int j = nodes-1; j > 0; j--) {
            if(node[j].nchild >= MAX_CHILD) continue;

            for(int k = 0; k < new_nodes; k++)
                if(random() % 100 < pct_edges) {
                    int cn = node[j].nchild++;
                    node[k+nodes].nparent++;

                    node[j].child[cn] = k+nodes;
                    if(cn+1 >= MAX_CHILD)
                        break;
                }
        }

        nodes += new_nodes; /* Accumulate into old node set.  */
    }

    return node;
}

void print_dag(int nodes, node_t *node) {
    printf("digraph {\n");
    for(int i=0; i<nodes; i++) {
        printf("// node %d (%d parents)\n", node[i].id, node[i].nparent);
        for(int j=0; j<node[i].nchild; j++) {
            printf ("  %d -> %d;\n", node[i].id, node[node[i].child[j]].id);
        }
    }
    printf("}\n");
}

// recursively add non-lazy children of current task
void add_task(node_t *dag, task_t *task, task_t *start, int i) {
    node_t *n = dag + i;
    int nchild = 0;

    // set => already added deps
    if(set_task_info(task+i+1, dag+i) != NULL) {
        return;
    }
    link_task(task+i+1, start); // link all new tasks to start

    if(dag[i].id < 0) { // don't (yet) add children of lazy node
        return;
    }
    for(int j=0; j<n->nchild; j++) {
        int c = n->child[j];
        add_task(dag, task, start, c);
        nchild += link_task(&task[i+1], &task[c+1]);
    }

    /* only used in start-up phase, where it's done already.
    if(n->nparent == 0)
        link_task(task[0], task[i+1]);
        */
}

task_t *hash_node(void *x, void *runinfo) {
    node_t *n = (node_t *)x;
    node_t *dag = n - (n->id > 0 ? n->id : -n->id) + 1;
    task_t *task = (task_t *)runinfo;

    //printf("%d\n", n->id);
    if(n->id < 0) {
        task_t *start = start_task();
        n->id = -n->id; // activate!

        // Don't start running this until I'm done!
        link_task(task+n->id, start);
        for(int j=0; j<n->nchild; j++) {
            int ch = n->child[j];
            add_task(dag, task, start, ch);
            link_task(task+n->id, task+ch+1); // since these links enable self
        }
        return start;
    }
    int val = n->id < 0 ? -n->id : n->id;
    for(int j=0; j<n->nchild; j++) {
        node_t *child = dag + n->child[j];
        val = integerHash(val | (child->id << 16));
    }
#if USLEEP > 0
    usleep(USLEEP);
#endif

end:
    if(n->id < 100) // Don't go overboard with the printing.
        printf("# node %d ~> %04x\n", n->id, val);
    n->id = val;
    return NULL;
}

int check_hash(int i, node_t *dag) {
    int val = i+1;
    node_t *n = dag+i;

    for(int j=0; j<n->nchild; j++) {
        node_t *child = dag + n->child[j];
        val = integerHash(val | (child->id << 16));
    }
    if(val != n->id) {
        printf("# Error at node %d! got %04x, expected %04x\n", i, n->id, val);
        return 1;
    }
    return 0;
}

int check_dag(int nodes, node_t *dag) {
    for(int i=0; i<nodes; i++) {
        if(check_hash(i, dag)) {
            return 1;
        }
    }
    return 0;
}

int test_dag(int ranks, int pct_edges, int pct_lazy, int min_wid, int max_wid) {
    int wid[MAXRANK];
    int ret;
    int nodes = fill_widths(wid, ranks, min_wid, max_wid);
    node_t *dag = gen_dag(nodes, ranks, pct_edges, pct_lazy, wid);
    task_t *task = dag ? new_tasks(nodes+1) : NULL;

    //print_dag(nodes, dag);

    if(dag == NULL || task == NULL) {
        printf("# memory error!\n");
        return 1;
    }
    task_t *start = start_task();

    // create task_t-s from node_t-s
    for(int i=0; i<nodes; i++) {
        if(dag[i].nparent == 0) { 
            add_task(dag, task, start, i);
            link_task(&task[0], &task[i+1]);
        }
    }
    printf("# %d nodes\n", nodes);

    exec_dag(start, hash_node, task);

    del_tasks(nodes+1, task);

    ret = check_dag(nodes, dag);
    free(dag);
    return ret;
}

int main(int argc, char *argv[]) {
    //srandom(time(NULL));
    srandom(12);

    printf("1..10\n");
    check("random dag has correct execution order", test_dag(5, 30, 0, 1, 5));
    check("random dag has correct execution order", test_dag(20, 30, 0, 1, 5));
    check("random dag has correct execution order", test_dag(80, 30, 0, 1, 5));
    check("random dag has correct execution order", test_dag(160, 30, 0, 1, 5));
    check("random dag has correct execution order", test_dag(320, 30, 0, 1, 5));

    check("lazy dag has correct execution order", test_dag(5, 30, 20, 1, 5));
    check("lazy dag has correct execution order", test_dag(20, 30, 20, 1, 5));
    check("lazy dag has correct execution order", test_dag(80, 30, 20, 1, 5));
    check("lazy dag has correct execution order", test_dag(160, 30, 20, 1, 5));
    check("lazy dag has correct execution order", test_dag(320, 30, 20, 1, 5));

    return 0;
}
