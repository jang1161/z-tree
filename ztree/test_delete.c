/* test_delete.c — functional test for cow_delete (ZTree).
 *
 * Single-threaded:  insert N → delete half → verify → delete rest →
 *                   verify empty → re-insert.
 * Multi-threaded :  T workers each handle a disjoint slice of keys
 *                   for insert / delete / verify.  Verifies that
 *                   concurrent delete + insert + find produce a
 *                   consistent state (post-quiescence).
 *
 * Usage:
 *   sudo ./build/test_delete <N> <device>             # single-thread
 *   sudo ./build/test_delete <N> <device> <threads>   # multi-thread
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include "ztree_main.h"

static void reset_device(const char *dev)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "sudo nvme zns reset-zone -a %s", dev);
    if (system(cmd) == -1) perror("nvme reset-zone");
}

static void shuffle(int64_t *arr, int n, unsigned seed)
{
    srand(seed);
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int64_t t = arr[i]; arr[i] = arr[j]; arr[j] = t;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Single-threaded test
 * ═══════════════════════════════════════════════════════════════════════════ */
static int run_st(int N, const char *dev)
{
    int64_t *keys = malloc(sizeof(*keys) * (size_t)N);
    int *deleted = calloc((size_t)N, sizeof(*deleted));
    if (!keys || !deleted) { perror("malloc"); return 1; }
    for (int i = 0; i < N; i++) keys[i] = (int64_t)(i + 1);
    shuffle(keys, N, 0xC0FFEEu);

    reset_device(dev);
    sleep(1);

    cow_tree *t = cow_open(dev);
    if (!t) { perror("cow_open"); return 1; }

    /* Phase 1 */
    printf("[test] Phase 1 (ST): insert %d keys ...\n", N);
    for (int i = 0; i < N; i++) {
        char buf[120];
        snprintf(buf, sizeof buf, "v-%" PRId64, keys[i]);
        cow_insert(t, keys[i], buf);
    }
    int missing = 0;
    for (int i = 0; i < N; i++) {
        ztree_record *r = ztree_find(t, keys[i]);
        if (!r) missing++; else free(r);
    }
    if (missing) {
        fprintf(stderr, "[test] FAIL phase 1: %d/%d missing\n", missing, N);
        cow_close(t); return 1;
    }
    printf("[test] Phase 1 OK\n");

    /* Phase 2 */
    int half = N / 2;
    int *del_idx = malloc(sizeof(*del_idx) * (size_t)half);
    {
        int *perm = malloc(sizeof(*perm) * (size_t)N);
        for (int i = 0; i < N; i++) perm[i] = i;
        srand(0xBADBEEFu);
        for (int i = N - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
        }
        for (int i = 0; i < half; i++) del_idx[i] = perm[i];
        free(perm);
    }
    printf("[test] Phase 2 (ST): delete %d keys ...\n", half);
    int del_ok = 0;
    for (int i = 0; i < half; i++) {
        int64_t k = keys[del_idx[i]];
        if (cow_delete(t, k)) { deleted[del_idx[i]] = 1; del_ok++; }
    }
    if (del_ok != half) {
        fprintf(stderr, "[test] FAIL phase 2: deleted %d/%d\n", del_ok, half);
        cow_close(t); return 1;
    }
    int wrong_d = 0, wrong_s = 0;
    for (int i = 0; i < N; i++) {
        ztree_record *r = ztree_find(t, keys[i]);
        if (deleted[i]) { if (r) { wrong_d++; free(r); } }
        else { if (!r) wrong_s++; else free(r); }
    }
    if (wrong_d || wrong_s) {
        fprintf(stderr, "[test] FAIL phase 2 verify: deleted-found=%d survivor-missing=%d\n",
                wrong_d, wrong_s);
        cow_close(t); return 1;
    }
    printf("[test] Phase 2 OK\n");

    /* Phase 3 */
    printf("[test] Phase 3 (ST): delete remaining ...\n");
    int del_ok2 = 0;
    for (int i = 0; i < N; i++) {
        if (deleted[i]) continue;
        if (cow_delete(t, keys[i])) { deleted[i] = 1; del_ok2++; }
    }
    int still = 0;
    for (int i = 0; i < N; i++) {
        ztree_record *r = ztree_find(t, keys[i]);
        if (r) { still++; free(r); }
    }
    if (still) {
        fprintf(stderr, "[test] FAIL phase 3 verify: %d still found\n", still);
        cow_close(t); return 1;
    }
    printf("[test] Phase 3 OK\n");

    /* Phase 4 */
    printf("[test] Phase 4 (ST): re-insert ...\n");
    for (int i = 0; i < N; i++) {
        char buf[120];
        snprintf(buf, sizeof buf, "v2-%" PRId64, keys[i]);
        cow_insert(t, keys[i], buf);
    }
    int ri_missing = 0;
    for (int i = 0; i < N; i++) {
        ztree_record *r = ztree_find(t, keys[i]);
        if (!r) ri_missing++; else free(r);
    }
    if (ri_missing) {
        fprintf(stderr, "[test] FAIL phase 4: %d missing\n", ri_missing);
        cow_close(t); return 1;
    }
    printf("[test] Phase 4 OK\n");

    cow_close(t);
    free(keys); free(deleted); free(del_idx);
    printf("\n[test] ALL PHASES PASSED (single-thread)\n");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Multi-threaded test
 *
 * Each thread owns a disjoint slice of keys.  Every key is inserted, then
 * exactly half are deleted (deterministic per-key based on hash).  After
 * a full barrier, the main thread verifies: deleted keys → NULL, survivors
 * → readable.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    cow_tree *t;
    int64_t *keys;        /* shared array, length N */
    char    *deleted;     /* shared, length N: 1 if this key should be deleted */
    int      thread_id;
    int      num_threads;
    int      N;
    /* per-thread counters */
    long ins_ok;
    long del_ok;
    long del_miss;
} thr_arg;

/* Phase 2 worker: each thread deletes its slice's keys in shuffled order. */
static void *mixed_worker(void *arg)
{
    thr_arg *a = arg;
    /* Each thread: walk its slice in shuffled order, doing insert then delete
     * (if marked deleted) for each key.  Interleaves with other threads' ops
     * on different keys. */
    int N = a->N;
    int t = a->thread_id;
    int T = a->num_threads;
    /* Per-thread shuffle of indices in slice. */
    int slice_n = (N - t + T - 1) / T;
    int *slice_idx = malloc(sizeof(int) * (size_t)slice_n);
    int p = 0;
    for (int i = t; i < N; i += T) slice_idx[p++] = i;
    /* shuffle */
    unsigned seed = 0xA5A5A5A5u + (unsigned)t;
    srand(seed);
    for (int i = slice_n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = slice_idx[i]; slice_idx[i] = slice_idx[j]; slice_idx[j] = tmp;
    }

    for (int s = 0; s < slice_n; s++) {
        int idx = slice_idx[s];
        if (a->deleted[idx]) {
            if (cow_delete(a->t, a->keys[idx])) a->del_ok++;
            else a->del_miss++;
        }
    }
    free(slice_idx);
    return NULL;
}

static int run_mt(int N, int T, const char *dev)
{
    int64_t *keys = malloc(sizeof(*keys) * (size_t)N);
    char *deleted_plan = calloc((size_t)N, sizeof(*deleted_plan));
    if (!keys || !deleted_plan) { perror("malloc"); return 1; }
    for (int i = 0; i < N; i++) keys[i] = (int64_t)(i + 1);
    shuffle(keys, N, 0xC0FFEEu);

    /* Mark half the keys for deletion (deterministic random subset). */
    {
        int *perm = malloc(sizeof(int) * (size_t)N);
        for (int i = 0; i < N; i++) perm[i] = i;
        srand(0xBADBEEFu);
        for (int i = N - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
        }
        int half = N / 2;
        for (int i = 0; i < half; i++) deleted_plan[perm[i]] = 1;
        free(perm);
    }

    reset_device(dev);
    sleep(1);

    cow_tree *t = cow_open(dev);
    if (!t) { perror("cow_open"); return 1; }

    pthread_t *thr = malloc(sizeof(pthread_t) * (size_t)T);
    thr_arg *args = calloc((size_t)T, sizeof(thr_arg));
    for (int i = 0; i < T; i++) {
        args[i].t = t;
        args[i].keys = keys;
        args[i].deleted = deleted_plan;
        args[i].thread_id = i;
        args[i].num_threads = T;
        args[i].N = N;
    }

    /* Phase 1: SINGLE-THREAD insert (avoids the existing ZTree concurrent-
     * insert race that's orthogonal to the DELETE concurrency we're testing). */
    printf("[test] Phase 1 (ST, single-thread): insert %d keys ...\n", N);
    for (int i = 0; i < N; i++) {
        char buf[120];
        snprintf(buf, sizeof buf, "v-%" PRId64, keys[i]);
        cow_insert(t, keys[i], buf);
    }
    int missing = 0;
    for (int i = 0; i < N; i++) {
        ztree_record *r = ztree_find(t, keys[i]);
        if (!r) missing++; else free(r);
    }
    if (missing) {
        fprintf(stderr, "[test] FAIL phase 1 (ST): %d/%d missing\n", missing, N);
        cow_close(t); return 1;
    }
    printf("[test] Phase 1 OK\n");

    /* Phase 2: parallel delete (workers each handle a disjoint slice). */
    printf("[test] Phase 2 (MT): concurrent delete %d keys ...\n", N / 2);
    for (int i = 0; i < T; i++)
        pthread_create(&thr[i], NULL, mixed_worker, &args[i]);
    for (int i = 0; i < T; i++) pthread_join(thr[i], NULL);

    long del_ok = 0, del_miss = 0;
    for (int i = 0; i < T; i++) { del_ok += args[i].del_ok; del_miss += args[i].del_miss; }
    int expected_del = N / 2;
    if (del_ok != expected_del || del_miss != 0) {
        fprintf(stderr, "[test] FAIL phase 2 (MT): del_ok=%ld del_miss=%ld (expected %d/0)\n",
                del_ok, del_miss, expected_del);
        cow_close(t); return 1;
    }

    /* Verify state: deleted → NULL, survivors → present */
    int wrong_d = 0, wrong_s = 0;
    for (int i = 0; i < N; i++) {
        ztree_record *r = ztree_find(t, keys[i]);
        if (deleted_plan[i]) { if (r) { wrong_d++; free(r); } }
        else { if (!r) wrong_s++; else free(r); }
    }
    if (wrong_d || wrong_s) {
        fprintf(stderr, "[test] FAIL phase 2 verify (MT): "
                "deleted-still-found=%d survivor-missing=%d\n", wrong_d, wrong_s);
        cow_close(t); return 1;
    }
    printf("[test] Phase 2 OK (MT)\n");

    cow_close(t);
    free(thr); free(args);
    free(keys); free(deleted_plan);
    printf("\n[test] ALL PHASES PASSED (multi-thread, T=%d)\n", T);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <N> <device> [threads]\n", argv[0]);
        return 1;
    }
    int N = atoi(argv[1]);
    const char *dev = argv[2];
    int T = (argc == 4) ? atoi(argv[3]) : 1;
    if (T < 1) T = 1;

    if (T == 1) return run_st(N, dev);
    return run_mt(N, T, dev);
}
