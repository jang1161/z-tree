/* test_zns_gc.c — standalone test for ZNS region GC (cow_gc_zns).
 *
 * Exercises the dynamic variant's ZNS GC in isolation, without YCSB.
 *
 * Scenario:
 *   1. Insert N keys (pass=0).
 *   2. Force CNS-resident leaves to ZNS via cow_evict_cns_leaves + cow_gc_cns
 *      so subsequent updates CoW on ZNS.
 *   3. Run K update passes — each rewrites every key with a new value.
 *      Each update CoW's the leaf, leaving a stale page in the prev zone.
 *   3a. cow_evict_cns_leaves + cow_gc_cns again — updates spill some leaves
 *       back onto CNS under zone-write-lock contention; bring them back to
 *       ZNS so phase 4 sees the maximal set of ZNS-resident leaves.
 *   4. Call cow_gc_zns(t).  Its own stderr line prints victims /
 *      pages_migrated / zns_phys before→after — that IS the measurement.
 *   5. Verify: ztree_find every key, value must match pass=K (the latest).
 *
 * Pass criteria: missing == 0 && mismatch == 0 (no data loss after GC).
 *
 * Usage:
 *   sudo CTREE_DYNAMIC_ZNS_GC=1 CTREE_DYNAMIC_GC_INTERVAL_MS=0 \
 *        ./build/test_zns_gc <keys> <update_passes> <threads> <device>
 *
 *   CTREE_DYNAMIC_ZNS_GC=1     enable cow_gc_zns (default-off, env-gated).
 *   CTREE_DYNAMIC_GC_INTERVAL_MS=0
 *                              disable the periodic CNS GC thread (default
 *                              1000ms).  The test does its own explicit
 *                              evict+gc_cns in phase 2 — leaving the
 *                              background thread on would race against the
 *                              update phase and add noise to the logs.
 *
 * Build:
 *   gcc -O2 -g -Wall -Wextra -std=c11 -pthread -I./ctree \
 *       ctree/ctree_nlt.c ctree/ctree_zone.c \
 *       ctree/variants/dynamic/ctree_main.c \
 *       ctree/variants/dynamic/test_zns_gc.c \
 *       -o build/test_zns_gc -lzbd -lnvme -lpthread
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ctree_main.h"

static int      g_num_threads;
static int     *g_keys;
static int      g_keys_count;
static int      g_pass;
static _Atomic int g_cursor;

/* Verify-phase shared state. */
static cow_tree           *g_verify_tree;
static int                 g_verify_expected_pass;
static _Atomic int         g_verify_cursor;
static _Atomic uint64_t    g_verify_found, g_verify_missing, g_verify_mismatch;

static void shuffle(int *a, int n, unsigned seed)
{
    srand(seed);
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = a[i]; a[i] = a[j]; a[j] = tmp;
    }
}

/* Pack (key, pass) into the fixed 120-byte value buffer, zero-padded.
 * Verifier reproduces the same packing and memcmps. */
static void pack_value(int key, int pass, char buf[120])
{
    int n = snprintf(buf, 120, "k=%d;p=%d", key, pass);
    if (n < 0) n = 0;
    if (n > 120) n = 120;
    if (n < 120) memset(buf + n, 0, (size_t)(120 - n));
}

static int value_matches(int key, int expected_pass, const char *got)
{
    char expected[120];
    pack_value(key, expected_pass, expected);
    return memcmp(expected, got, 120) == 0;
}

static void *verify_worker(void *arg)
{
    (void)arg;
    int idx;
    while ((idx = atomic_fetch_add(&g_verify_cursor, 1)) < g_keys_count) {
        ztree_record *r = ztree_find(g_verify_tree, g_keys[idx]);
        if (!r) {
            atomic_fetch_add(&g_verify_missing, 1);
            continue;
        }
        if (value_matches(g_keys[idx], g_verify_expected_pass, r->value))
            atomic_fetch_add(&g_verify_found, 1);
        else
            atomic_fetch_add(&g_verify_mismatch, 1);
        free(r);
    }
    return NULL;
}

static void *worker(void *arg)
{
    cow_tree *t = (cow_tree *)arg;
    int idx;
    while ((idx = atomic_fetch_add(&g_cursor, 1)) < g_keys_count) {
        char buf[120];
        pack_value(g_keys[idx], g_pass, buf);
        cow_insert(t, g_keys[idx], buf);
    }
    return NULL;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void run_pass(cow_tree *t, pthread_t *thr, int pass)
{
    g_pass = pass;
    atomic_store(&g_cursor, 0);
    double t0 = now_sec();
    for (int i = 0; i < g_num_threads; i++)
        pthread_create(&thr[i], NULL, worker, t);
    for (int i = 0; i < g_num_threads; i++)
        pthread_join(thr[i], NULL);
    double t1 = now_sec();
    printf("  pass %2d: %.2fs (%.0f ops/s)\n",
           pass, t1 - t0,
           (t1 > t0) ? (double)g_keys_count / (t1 - t0) : 0.0);
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr,
                "Usage: %s <keys> <update_passes> <threads> <device>\n",
                argv[0]);
        return 1;
    }
    int N = atoi(argv[1]);
    int K = atoi(argv[2]);
    g_num_threads = atoi(argv[3]);
    const char *dev = argv[4];

    if (!getenv("CTREE_DYNAMIC_ZNS_GC")
        || getenv("CTREE_DYNAMIC_ZNS_GC")[0] != '1') {
        fprintf(stderr,
                "[test_zns_gc] WARNING: CTREE_DYNAMIC_ZNS_GC=1 not set — "
                "cow_gc_zns will return 0 (gated off).\n");
    }

    /* Reset ZNS at start; CNS file is recreated by cow_open. */
    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "sudo nvme zns reset-zone -a %s >/dev/null", dev);
    if (system(cmd) != 0) { /* ignore */ }
    sleep(1);

    cow_tree *t = cow_open(dev);
    if (!t) { fprintf(stderr, "cow_open failed\n"); return 1; }

    int *keys = malloc(sizeof(int) * (size_t)N);
    if (!keys) { fprintf(stderr, "OOM\n"); cow_close(t); return 1; }
    for (int i = 0; i < N; i++) keys[i] = i + 1;
    shuffle(keys, N, 0xC0FFEEu);
    g_keys = keys;
    g_keys_count = N;

    pthread_t *thr = malloc(sizeof(pthread_t) * (size_t)g_num_threads);
    if (!thr) { fprintf(stderr, "OOM\n"); free(keys); cow_close(t); return 1; }

    printf("\n========================================\n");
    printf("  ZNS GC standalone test\n");
    printf("    keys           = %d\n", N);
    printf("    update_passes  = %d\n", K);
    printf("    threads        = %d\n", g_num_threads);
    printf("    device         = %s\n", dev);
    printf("========================================\n");

    /* ── Phase 1: initial insert (pass=0). */
    printf("\n[phase 1] initial insert\n");
    run_pass(t, thr, 0);

    /* ── Phase 2: push leaves to ZNS so updates CoW there. */
    printf("\n[phase 2] cow_evict_cns_leaves + cow_gc_cns\n");
    cow_phase_mark(t, "begin:evict-pre");
    size_t evicted   = cow_evict_cns_leaves(t);
    size_t freed_cns = cow_gc_cns(t);
    cow_phase_mark(t, "end:evict-pre");
    printf("  evicted=%zu  cns freed=%zu KB\n", evicted, freed_cns / 1024);

    /* ── Phase 3: K update passes, each rewriting every key. */
    printf("\n[phase 3] %d update pass(es)\n", K);
    for (int p = 1; p <= K; p++)
        run_pass(t, thr, p);

    /* ── Phase 3a: post-update evict + gc_cns.  Update-time contention spills
     *    some leaves back to CNS; bring them back to ZNS so cow_gc_zns sees
     *    the full set of ZNS-resident leaves and so CNS phys is reclaimed. */
    printf("\n[phase 3a] post-update cow_evict_cns_leaves + cow_gc_cns\n");
    cow_phase_mark(t, "begin:evict-post");
    size_t evicted2   = cow_evict_cns_leaves(t);
    size_t freed_cns2 = cow_gc_cns(t);
    cow_phase_mark(t, "end:evict-post");
    printf("  evicted=%zu  cns freed=%zu KB\n", evicted2, freed_cns2 / 1024);

    /* ── Phase 4: ZNS GC.  cow_gc_zns itself logs victims / pages_migrated
     *    / zns_phys before→after on stderr, which is the measurement. */
    printf("\n[phase 4] cow_gc_zns\n");
    cow_phase_mark(t, "begin:zns_gc");
    size_t freed_zns = cow_gc_zns(t);
    cow_phase_mark(t, "end:zns_gc");
    printf("  freed_zns = %zu KB  (see cow_gc_zns stderr line above)\n",
           freed_zns / 1024);

    /* ── Phase 5: verify every key resolves to pass=K's value. */
    printf("\n[phase 5] verify (%d threads)\n", g_num_threads);
    g_verify_tree = t;
    g_verify_expected_pass = (K > 0) ? K : 0;
    atomic_store(&g_verify_cursor, 0);
    atomic_store(&g_verify_found, 0);
    atomic_store(&g_verify_missing, 0);
    atomic_store(&g_verify_mismatch, 0);
    double v0 = now_sec();
    for (int i = 0; i < g_num_threads; i++)
        pthread_create(&thr[i], NULL, verify_worker, NULL);
    for (int i = 0; i < g_num_threads; i++)
        pthread_join(thr[i], NULL);
    double v1 = now_sec();
    uint64_t found    = atomic_load(&g_verify_found);
    uint64_t missing  = atomic_load(&g_verify_missing);
    uint64_t mismatch = atomic_load(&g_verify_mismatch);
    printf("  found=%llu  missing=%llu  mismatch=%llu  (%.2fs, %.0f find/s)\n",
           (unsigned long long)found, (unsigned long long)missing,
           (unsigned long long)mismatch,
           v1 - v0,
           (v1 > v0) ? (double)N / (v1 - v0) : 0.0);
    int ok = (missing == 0 && mismatch == 0);
    printf("  %s\n", ok ? "✓ PASS" : "✗ FAIL");

    cow_close(t);
    free(keys);
    free(thr);

    return ok ? 0 : 1;
}
