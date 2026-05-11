/* test_dynamic_cns.c — burst / evict / GC cycle for the dynamic variant.
 *
 * Drives several insert bursts back-to-back, calling cow_evict_cns_leaves
 * and cow_gc_cns between them.  The trace CSV at /tmp/ctree_dynamic_trace.csv
 * captures the sawtooth in cns_phys_bytes.
 *
 * Usage:
 *   sudo ./build/test_dynamic_cns <keys_per_burst> <num_bursts> <threads> <device>
 *
 * Example: sudo ./build/test_dynamic_cns 200000 4 64 /dev/nvme3n2
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
static _Atomic int g_cursor;

/* Delete-phase shared state (separate cursor so the same worker pattern
 * can drive either insert or delete depending on which callback runs). */
static int     *g_del_keys;
static int      g_del_count;
static _Atomic int      g_del_cursor;
static _Atomic uint64_t g_del_succeeded;

static void shuffle(int *a, int n, unsigned seed)
{
    srand(seed);
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = a[i]; a[i] = a[j]; a[j] = t;
    }
}

static void *worker(void *arg)
{
    cow_tree *t = (cow_tree *)arg;
    int idx;
    while ((idx = atomic_fetch_add(&g_cursor, 1)) < g_keys_count) {
        char buf[120];
        snprintf(buf, sizeof buf, "v-%d", g_keys[idx]);
        cow_insert(t, g_keys[idx], buf);
    }
    return NULL;
}

/* Delete worker — drives concurrent cow_delete to exercise merge-cascade
 * paths (the leak target).  Counts return-value 1 so we can detect any
 * spurious not-found (caused by RACE-style premature exits, etc.). */
static void *delete_worker(void *arg)
{
    cow_tree *t = (cow_tree *)arg;
    int idx;
    while ((idx = atomic_fetch_add(&g_del_cursor, 1)) < g_del_count) {
        if (cow_delete(t, g_del_keys[idx]))
            atomic_fetch_add(&g_del_succeeded, 1);
    }
    return NULL;
}

static size_t cns_size_kb(void)
{
    struct stat st;
    if (stat("/mnt/cns/nodes.dat", &st) != 0) return 0;
    return (size_t)st.st_blocks * 512 / 1024;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr,
                "Usage: %s <keys_per_burst> <num_bursts> <threads> <device>\n",
                argv[0]);
        return 1;
    }
    int keys_per_burst = atoi(argv[1]);
    int num_bursts     = atoi(argv[2]);
    g_num_threads      = atoi(argv[3]);
    const char *dev    = argv[4];

    /* Reset ZNS once at start (CNS handled by cow_open's ftruncate). */
    char cmd[256];
    snprintf(cmd, sizeof cmd, "sudo nvme zns reset-zone -a %s >/dev/null", dev);
    if (system(cmd) != 0) { /* ignore */ }
    sleep(1);

    cow_tree *t = cow_open(dev);
    if (!t) { fprintf(stderr, "cow_open failed\n"); return 1; }

    /* Pre-generate the union of all keys to insert (each burst inserts
     * a disjoint range so we don't hit duplicates). */
    int total = keys_per_burst * num_bursts;
    int *all = malloc(sizeof(int) * (size_t)total);
    for (int i = 0; i < total; i++) all[i] = i + 1;
    shuffle(all, total, 0xC0FFEEu);

    pthread_t *thr = malloc(sizeof(pthread_t) * (size_t)g_num_threads);

    printf("\n========================================\n");
    printf("  burst-evict-gc test\n");
    printf("    keys/burst  = %d\n", keys_per_burst);
    printf("    num_bursts  = %d\n", num_bursts);
    printf("    threads     = %d\n", g_num_threads);
    printf("    device      = %s\n", dev);
    printf("========================================\n");

    double t0 = now_sec();

    for (int b = 0; b < num_bursts; b++) {
        printf("\n[burst %d/%d] inserting %d keys...\n", b + 1, num_bursts, keys_per_burst);
        g_keys = all + (b * keys_per_burst);
        g_keys_count = keys_per_burst;
        atomic_store(&g_cursor, 0);

        double tb0 = now_sec();
        for (int i = 0; i < g_num_threads; i++)
            pthread_create(&thr[i], NULL, worker, t);
        for (int i = 0; i < g_num_threads; i++) pthread_join(thr[i], NULL);
        double tb1 = now_sec();
        printf("  burst%d done in %.2fs (%.0f ops/s)  cns_phys=%zu KB\n",
               b + 1, tb1 - tb0, keys_per_burst / (tb1 - tb0), cns_size_kb());

        const char *mode = getenv("MAINT_MODE") ?: "evict_gc";
        if (strcmp(mode, "none") == 0) {
            printf("[maint] MAINT_MODE=none — skip evict + gc\n");
        } else if (strcmp(mode, "gc_only") == 0) {
            printf("[maint] MAINT_MODE=gc_only — gc without evict\n");
            size_t freed = cow_gc_cns(t);
            printf("  freed %zu KB; cns_phys=%zu KB\n", freed / 1024, cns_size_kb());
        } else if (strcmp(mode, "evict_only") == 0) {
            printf("[maint] MAINT_MODE=evict_only — evict without gc\n");
            size_t evicted = cow_evict_cns_leaves(t);
            printf("  evicted %zu leaves; cns_phys=%zu KB\n", evicted, cns_size_kb());
        } else {
            printf("[evict] migrating CNS-resident leaves to ZNS...\n");
            size_t evicted = cow_evict_cns_leaves(t);
            printf("  evicted %zu leaves; cns_phys=%zu KB\n", evicted, cns_size_kb());
            printf("[gc]    punching orphan CNS slots...\n");
            size_t freed = cow_gc_cns(t);
            printf("  freed %zu KB; cns_phys=%zu KB\n", freed / 1024, cns_size_kb());
        }

        /* Verify #2: post-eviction CNS should hold INTERNALS only (0 leaves) */
        size_t ints, leaves;
        cow_count_cns_residents(t, &ints, &leaves);
        printf("[verify] CNS residents after cycle %d: internals=%zu  leaves=%zu",
               b + 1, ints, leaves);
        if (leaves != 0 && strcmp(getenv("MAINT_MODE") ?: "evict_gc", "evict_gc") == 0
                       && strcmp(getenv("MAINT_MODE") ?: "evict_gc", "evict_only") != 0)
            printf("  ⚠️  expected 0 leaves after eviction!");
        printf("\n");
    }

    double t1 = now_sec();
    printf("\n[total] %.2fs elapsed (insert phase)\n", t1 - t0);

    /* Verify #1: lookup every key inserted, count found vs not_found. */
    printf("\n[verify] looking up all %d inserted keys (single-thread)...\n", total);
    double v0 = now_sec();
    size_t found = 0, missing = 0;
    for (int i = 0; i < total; i++) {
        ztree_record *r = ztree_find(t, all[i]);
        if (r) { found++; free(r); }
        else { missing++; }
    }
    double v1 = now_sec();
    printf("  found=%zu  not_found=%zu  in %.2fs (%.0f find/s)  %s\n",
           found, missing, v1 - v0, (double)total / (v1 - v0),
           missing == 0 ? "✓" : "✗ DATA LOSS");

    /* ── Delete phase ─────────────────────────────────────────────────────
     * Drives concurrent cow_delete to exercise merge-cascade / root
     * collapse / root-leaf-empty paths.  Then forces evict+GC and reads
     * cns_phys before/after to confirm the retire fix actually frees the
     * physical CNS blocks (without the fix, cns_phys would stay flat
     * because merged siblings keep bitmap=1 → GC skips their slots). */
    const char *df_env = getenv("DELETE_FRACTION");
    double delete_fraction = df_env ? atof(df_env) : 0.5;
    if (delete_fraction <= 0.0) {
        printf("\n[delete] DELETE_FRACTION=%g — skip delete phase\n", delete_fraction);
    } else {
        if (delete_fraction > 1.0) delete_fraction = 1.0;
        int delete_count = (int)((double)total * delete_fraction);
        if (delete_count < 1) delete_count = 1;

        size_t cns_before_del = cns_size_kb();
        size_t cns_int_before, cns_leaf_before;
        cow_count_cns_residents(t, &cns_int_before, &cns_leaf_before);

        printf("\n========================================\n");
        printf("  delete phase\n");
        printf("    fraction       = %.0f%%  (%d / %d keys)\n",
               delete_fraction * 100.0, delete_count, total);
        printf("    threads        = %d\n", g_num_threads);
        printf("    cns_phys begin = %zu KB  (CNS residents: int=%zu leaf=%zu)\n",
               cns_before_del, cns_int_before, cns_leaf_before);
        printf("========================================\n");

        /* all[] is already shuffled; the prefix [0..delete_count) is a
         * uniformly-random subset of keys. */
        g_del_keys = all;
        g_del_count = delete_count;
        atomic_store(&g_del_cursor, 0);
        atomic_store(&g_del_succeeded, 0);

        double td0 = now_sec();
        for (int i = 0; i < g_num_threads; i++)
            pthread_create(&thr[i], NULL, delete_worker, t);
        for (int i = 0; i < g_num_threads; i++) pthread_join(thr[i], NULL);
        double td1 = now_sec();
        uint64_t del_succ = atomic_load(&g_del_succeeded);
        printf("\n  deleted %llu/%d in %.2fs (%.0f ops/s)\n",
               (unsigned long long)del_succ, delete_count,
               td1 - td0, (double)delete_count / (td1 - td0));
        printf("  cns_phys: %zu KB (begin) → %zu KB (post-delete, pre-GC)\n",
               cns_before_del, cns_size_kb());

        /* Force one evict+GC cycle so retired slots get punched. */
        printf("\n[delete-maint] evicting CNS-resident leaves...\n");
        size_t evicted = cow_evict_cns_leaves(t);
        printf("  evicted %zu leaves; cns_phys=%zu KB\n",
               evicted, cns_size_kb());
        printf("[delete-maint] running GC...\n");
        size_t freed = cow_gc_cns(t);
        printf("  freed %zu KB; cns_phys=%zu KB\n",
               freed / 1024, cns_size_kb());

        size_t cns_int_after, cns_leaf_after;
        cow_count_cns_residents(t, &cns_int_after, &cns_leaf_after);
        size_t cns_after_del = cns_size_kb();
        printf("\n[delete-result]\n");
        printf("  cns_phys: %zu KB → %zu KB  (Δ = %+ld KB)\n",
               cns_before_del, cns_after_del,
               (long)cns_after_del - (long)cns_before_del);
        printf("  CNS residents: int %zu → %zu   leaf %zu → %zu\n",
               cns_int_before, cns_int_after,
               cns_leaf_before, cns_leaf_after);
        if (cns_after_del < cns_before_del)
            printf("  ✓ physical CNS reclaimed (retire fix working)\n");
        else
            printf("  ⚠ cns_phys did NOT drop — check for retire leaks\n");

        /* Verify #2: deleted keys must be missing, survivors must be found. */
        printf("\n[verify] post-delete lookups (single-thread)...\n");
        double vv0 = now_sec();
        size_t del_stale = 0, del_clean = 0;
        size_t sur_found = 0, sur_lost = 0;
        for (int i = 0; i < total; i++) {
            ztree_record *r = ztree_find(t, all[i]);
            if (i < delete_count) {
                if (r) { del_stale++; free(r); }
                else   { del_clean++; }
            } else {
                if (r) { sur_found++; free(r); }
                else   { sur_lost++; }
            }
        }
        double vv1 = now_sec();
        printf("  deleted keys : found=%zu  not_found=%zu  %s\n",
               del_stale, del_clean,
               del_stale == 0 ? "✓" : "✗ STALE DATA");
        printf("  survivor keys: found=%zu  not_found=%zu  %s\n",
               sur_found, sur_lost,
               sur_lost == 0 ? "✓" : "✗ DATA LOSS");
        printf("  lookup elapsed: %.2fs (%.0f find/s)\n",
               vv1 - vv0, (double)total / (vv1 - vv0));
    }

    cow_close(t);
    free(all);
    free(thr);

    printf("\nTrace saved at /tmp/ctree_dynamic_trace.csv\n");
    printf("Plot it with:  python3 -c \""
           "import pandas as pd, matplotlib.pyplot as plt; "
           "df=pd.read_csv('/tmp/ctree_dynamic_trace.csv'); "
           "plt.plot(df.time_sec, df.cns_phys_bytes/1024/1024); "
           "plt.xlabel('s'); plt.ylabel('CNS phys MB'); "
           "plt.savefig('/tmp/sawtooth.png',dpi=120)\"\n");
    return 0;
}
