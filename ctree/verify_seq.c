/* verify_seq.c — quick verifier: sequential insert N keys, then find all.
 * Usage: sudo BENCH_SEQUENTIAL=1 ./build/verify_seq <N> <threads> <dev>
 *
 * Reports: how many of the N keys are findable. If << N, base ctree's
 * concurrent insert is losing keys. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ctree_main.h"

static int total_keys;
static int *all_keys;
static _Atomic int work_cursor;
static _Atomic int found_cnt;
static _Atomic int missing_cnt;

typedef struct { cow_tree *t; } arg_t;

static void *inserter(void *p) {
    arg_t *a = p;
    int idx;
    while ((idx = atomic_fetch_add(&work_cursor, 1)) < total_keys) {
        int key = all_keys[idx];
        char buf[120];
        snprintf(buf, sizeof(buf), "value-%d", key);
        cow_insert(a->t, key, buf);
    }
    return NULL;
}

static void *finder(void *p) {
    arg_t *a = p;
    int idx;
    while ((idx = atomic_fetch_add(&work_cursor, 1)) < total_keys) {
        int key = all_keys[idx];
        ztree_record *r = ztree_find(a->t, key);
        if (r) { atomic_fetch_add(&found_cnt, 1); free(r); }
        else   { atomic_fetch_add(&missing_cnt, 1); }
    }
    return NULL;
}

/* For debugging: walk keys 0..N-1 in order, mark found/missing, print pattern. */
static void dump_missing_pattern(cow_tree *t, int n, int limit) {
    int run_start = -1, run_len = 0, runs_printed = 0;
    int last_state = -1;  /* 0 = missing, 1 = found */
    for (int k = 0; k < n; k++) {
        ztree_record *r = ztree_find(t, k);
        int state = r ? 1 : 0;
        if (r) free(r);
        if (state != last_state) {
            if (run_len > 0 && runs_printed < limit) {
                printf("  keys[%d..%d] = %s (len=%d)\n",
                       run_start, run_start + run_len - 1,
                       last_state ? "FOUND" : "missing", run_len);
                runs_printed++;
            }
            run_start = k;
            run_len = 1;
            last_state = state;
        } else {
            run_len++;
        }
    }
    if (run_len > 0 && runs_printed < limit) {
        printf("  keys[%d..%d] = %s (len=%d)\n",
               run_start, run_start + run_len - 1,
               last_state ? "FOUND" : "missing", run_len);
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        printf("Usage: %s <N> <threads> <dev>\n", argv[0]);
        return 1;
    }
    total_keys = atoi(argv[1]);
    int nthreads = atoi(argv[2]);
    const char *dev = argv[3];

    all_keys = malloc(sizeof(int) * (size_t)total_keys);
    for (int i = 0; i < total_keys; i++) all_keys[i] = i;

    /* BENCH_SEQUENTIAL=1 (default): keep sequential order.
     * BENCH_SEQUENTIAL=0 or unset: Fisher-Yates shuffle (random). */
    const char *seq_env = getenv("BENCH_SEQUENTIAL");
    int sequential = seq_env && seq_env[0] == '1';
    if (!sequential) {
        srand(54321);
        for (int i = total_keys - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int tmp = all_keys[i]; all_keys[i] = all_keys[j]; all_keys[j] = tmp;
        }
    }
    printf("order:  %s\n", sequential ? "sequential" : "random (shuffled)");

    cow_tree *t = cow_open(dev);
    if (!t) { fprintf(stderr, "cow_open failed\n"); return 1; }

    /* INSERT phase */
    atomic_store(&work_cursor, 0);
    pthread_t tids[64];
    arg_t args = { .t = t };
    struct timespec ti0, ti1;
    clock_gettime(CLOCK_MONOTONIC, &ti0);
    for (int i = 0; i < nthreads; i++)
        pthread_create(&tids[i], NULL, inserter, &args);
    for (int i = 0; i < nthreads; i++)
        pthread_join(tids[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &ti1);
    double ins_sec = (ti1.tv_sec - ti0.tv_sec) + (ti1.tv_nsec - ti0.tv_nsec) / 1e9;
    printf("insert: %d keys in %.2f s (%.0f ops/s)\n",
           total_keys, ins_sec, total_keys / ins_sec);

    /* FIND phase */
    atomic_store(&work_cursor, 0);
    atomic_store(&found_cnt, 0);
    atomic_store(&missing_cnt, 0);
    clock_gettime(CLOCK_MONOTONIC, &ti0);
    for (int i = 0; i < nthreads; i++)
        pthread_create(&tids[i], NULL, finder, &args);
    for (int i = 0; i < nthreads; i++)
        pthread_join(tids[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &ti1);
    double find_sec = (ti1.tv_sec - ti0.tv_sec) + (ti1.tv_nsec - ti0.tv_nsec) / 1e9;
    int found = atomic_load(&found_cnt);
    int missing = atomic_load(&missing_cnt);
    printf("find:   %d keys in %.2f s\n", total_keys, find_sec);
    printf("  found   = %d (%.1f%%)\n", found, 100.0 * found / total_keys);
    printf("  missing = %d (%.1f%%)\n", missing, 100.0 * missing / total_keys);

    /* Dump first 40 runs of missing/found to reveal pattern. */
    if (getenv("VERIFY_DUMP")) {
        printf("\n[runs of missing/found in key order]\n");
        dump_missing_pattern(t, total_keys, 40);
    }

    cow_close(t);
    return 0;
}
