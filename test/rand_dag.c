// code adapted from ArjunShankar: http://stackoverflow.com/questions/12790337/generating-a-random-dag
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_CHILD 8

typedef struct NodeS node_t;
struct NodeS {
    int id;
    int nchild, nparent;
    int child[MAX_CHILD];
};

static const int MAXRANK = 500;

node_t *gen_graph(int ranks, int pct_edges,
                  int min_wid, int max_wid,
                  int *nnodes) {
    int i, j, k, nodes = 0;
    node_t *node;
    srandom(time(NULL));

    ranks = ranks < MAXRANK ? ranks : MAXRANK;
    int wid[MAXRANK];

    /* Determine width of each rank. */
    for(i = 0; i < ranks; i++) {
        wid[i] = min_wid + (random() % (max_wid - min_wid + 1));
        nodes += wid[i];
    }
    if( (node = malloc(sizeof(node_t)*nodes)) == NULL) {
        return NULL;
    }
    for(i=0; i<nodes; i++) { // initialize nodes.
        node[i].id = i;
        node[i].nchild = node[i].nparent = 0;
    }
    *nnodes = nodes;
    nodes = 0;

    for(i = 0; i < ranks; i++) {
        int new_nodes = wid[i];

        for(j = 0; j < nodes; j++) {
            if(node[j].nchild >= MAX_CHILD) continue;

            for(k = 0; k < new_nodes; k++)
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

/*
int main() {
    int nodes, i, j;
    node_t *node;
    node = gen_graph(4, 30, 1, 5, &nodes);
    if(node == NULL) {
        printf("Error allocating mem.\n");
        exit(1);
    }
    printf("digraph {\n");
    for(i=0; i<nodes; i++) {
        printf("// node %d (%d parents)\n", node[i].id, node[i].nparent);
        for(j=0; j<node[i].nchild; j++) {
            printf ("  %d -> %d;\n", i, node[i].child[j]);
        }
    }
    printf("}\n");
    free(node);
}*/
