#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <dag.h>
#include <time.h>
#include "test.h"

#define USLEEP 1000

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

node_t *gen_dag(int nodes, int ranks, int pct_edges, int wid[MAXRANK]) {
    node_t *node;

    if( (node = malloc(sizeof(node_t)*nodes)) == NULL) {
        return NULL;
    }
    for(int i=0; i<nodes; i++) { // initialize nodes.
        node[i].id = i;
        node[i].nchild = node[i].nparent = 0;
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

task_t *hash_node(void *x, void *runinfo) {
    node_t *n = (node_t *)x;
    task_t **task = (task_t **)runinfo;
    if(x == NULL) return NULL;

    int val = n->id;
    for(int j=0; j<n->nchild; j++) {
        node_t *child = (node_t *)get_info(task[n->child[j]]);
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
    int val = i;
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

int test_dag(int ranks, int pct_edges, int min_wid, int max_wid) {
    int wid[MAXRANK];
    int ret, edges=0;
    int nodes = fill_widths(wid, ranks, min_wid, max_wid);
    node_t *dag = gen_dag(nodes, ranks, pct_edges, wid);
    task_t **task = dag ? malloc(sizeof(task_t *)*nodes) : NULL;

    if(dag == NULL || task == NULL) {
        printf("# memory error!\n");
        return 1;
    }
    task_t *start = new_task(NULL, NULL);
    task_t *end   = new_task(NULL, NULL);

    // create task_t-s from node_t-s
    for(int i=0; i<nodes; i++) {
        if(dag[i].nchild == 0) {
            task[i] = new_task(&dag[i], start);
        } else {
            task[i] = new_task(&dag[i], NULL);
        }
        if(dag[i].nparent == 0) { 
            link_task(end, task[i]);
        }
    }
    for(int i=0; i<nodes; i++) {
        for(int j=0; j<dag[i].nchild; j++) {
            edges++;
            link_task(task[i], task[dag[i].child[j]]);
        }
    }
    printf("# %d nodes, %d edges\n", nodes, edges);

    exec_dag(start, hash_node, task);
    for(int i=0; i<nodes; i++) {
        del_task(task[i]);
    }
    del_task(end);

    ret = check_dag(nodes, dag);
    free(dag);
    free(task);
    return ret;
}

int main(int argc, char *argv[]) {
    //srandom(time(NULL));
    srandom(12);

    printf("1..5\n");
    check("random dag has correct execution order", test_dag(5, 30, 1, 5));
    check("random dag has correct execution order", test_dag(20, 30, 1, 5));
    check("random dag has correct execution order", test_dag(80, 30, 1, 5));
    check("random dag has correct execution order", test_dag(160, 30, 1, 5));
    check("random dag has correct execution order", test_dag(320, 30, 1, 5));

    return 0;
}
