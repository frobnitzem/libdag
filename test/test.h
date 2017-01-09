#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_num = 0;

#define check(msg, call) { \
    test_num++; \
    if(call) { \
        printf("not ok %d %s (line %d in %s)\n", \
                test_num, msg, __LINE__, __func__); \
    } else { \
        printf("ok %d %s\n", test_num, msg); \
    } \
}

#define xcheck(msg, call) { \
    test_num++; \
    if(call) { \
        printf("not ok %d %s # TODO (aka must fail)\n", test_num, msg ); \
    } else { \
        printf("ok %d %s (line %d in %s) # TODO (aka must fail)\n", \
                    test_num, msg, __LINE__, __func__); \
    } \
}

#define skip(msg, call) { \
    test_num++; \
    printf("ok %d # SKIP %s (line %d in %s)\n", test_num, msg, \
                                    __LINE__, __func__); \
}

