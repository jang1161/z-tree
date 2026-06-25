#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "ctree_main.h"

typedef struct
{
    ztree_t *t;
} thread_arg;

static int *all_keys;
static int total_keys;

static _Atomic int progress_count = 0;
static _Atomic int work_cursor = 0;
static _Atomic int verify_cursor = 0;
static _Atomic int found_cnt = 0;
static _Atomic int missing_cnt = 0;
static _Atomic bool monitor_stop = false;

/* Per-second throughput sampler: every 1s, log ops completed in that second.
 * Output to CTREE_TPUT_PATH (CSV) or stderr. Lets us see the throughput dip
 * during CNS GC cycles. */
static void *tput_monitor(void *arg)
{
    (void)arg;
    const char *path = getenv("CTREE_TPUT_PATH");
    FILE *fp = path ? fopen(path, "w") : stderr;
    if (!fp) fp = stderr;
    fprintf(fp, "sec,ops_this_sec,cumulative\n");
    fflush(fp);
    int prev = 0, sec = 0;
    while (!atomic_load(&monitor_stop))
    {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        int cur = atomic_load(&progress_count);
        sec++;
        fprintf(fp, "%d,%d,%d\n", sec, cur - prev, cur);
        fflush(fp);
        prev = cur;
    }
    if (fp != stderr) fclose(fp);
    return NULL;
}

static void *worker(void *arg)
{
    thread_arg *a = (thread_arg *)arg;

    int total = total_keys;
    int interval = total / 100;
    if (interval < 1)
    {
        interval = 1;
    }

    int idx;
    while ((idx = atomic_fetch_add(&work_cursor, 1)) < total)
    {
        int key = all_keys[idx];
        char buf[120];
        snprintf(buf, sizeof(buf), "value-%d", key);
        cow_insert(a->t, key, buf);

        int current = atomic_fetch_add(&progress_count, 1) + 1;
        if (current % interval == 0 || current == total)
        {
            printf("\r> Progress: %d / %d (%.1f%%)   ", current, total,
                   (double)current / total * 100.0);
            fflush(stdout);
        }
    }

    return NULL;
}

/* Post-insert integrity check: every inserted key must be findable. */
static void *verifier(void *arg)
{
    thread_arg *a = (thread_arg *)arg;
    int idx;
    while ((idx = atomic_fetch_add(&verify_cursor, 1)) < total_keys)
    {
        ztree_record *r = ztree_find(a->t, all_keys[idx]);
        if (r) { atomic_fetch_add(&found_cnt, 1); free(r); }
        else   { atomic_fetch_add(&missing_cnt, 1); }
    }
    return NULL;
}

static void reset_device(const char *dev_path)
{
    char cmd[640];

    /* F2FS variant: CNS lives on an F2FS mount over the dm-hyhost R-region.
     * NEVER `nvme zns reset-zone -a` / `blkdiscard` the whole device — that
     * would reset the R-region zones and corrupt the mounted F2FS.  Instead:
     *   (1) reset ONLY the S-region zones (offset >= CTREE_S_START_SECTORS),
     *   (2) clear the CNS node files (F2FS frees the blocks; cow_open also
     *       ftruncate(0)s each shard, this just gives a clean cns_phys base).
     * CTREE_S_START_SECTORS must equal the r_end used in f2fs_setup.sh. */
    const char *s_start = getenv("CTREE_S_START_SECTORS");
    if (!s_start || !*s_start) s_start = "8388608";   /* 4 GiB R-region default */
    snprintf(cmd, sizeof(cmd), "sudo blkzone reset -o %s %s", s_start, dev_path);
    int rc = system(cmd);
    if (rc != 0)
        fprintf(stderr,
                "WARNING: blkzone reset -o %s %s failed (rc=%d); ensure the "
                "S-region is empty (full reset BEFORE mounting F2FS)\n",
                s_start, dev_path, rc);

    const char *cns_dir = getenv("CTREE_CNS_DIR");
    if (!cns_dir || !*cns_dir) cns_dir = "/mnt/hyhost";
    snprintf(cmd, sizeof(cmd), "sudo rm -f %s/nodes.*.dat", cns_dir);
    rc = system(cmd);
    if (rc != 0)
        fprintf(stderr, "WARNING: rm %s/nodes.*.dat failed (rc=%d)\n", cns_dir, rc);
}

static void run_test(const char *dev_path, int num_threads)
{
    printf("Resetting devices...\n");
    reset_device(dev_path);
    sleep(1);

    atomic_store(&progress_count, 0);
    atomic_store(&work_cursor, 0);
    printf("\n===== Running with %d threads =====\n", num_threads);

    cow_tree *t = cow_open(dev_path);
    if (!t)
    {
        perror("cow_open failed");
        exit(1);
    }

    pthread_t *threads = malloc(sizeof(*threads) * (size_t)num_threads);
    thread_arg *args = malloc(sizeof(*args) * (size_t)num_threads);
    if (!threads || !args)
    {
        perror("malloc threads/args failed");
        free(threads);
        free(args);
        cow_close(t);
        exit(1);
    }

    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    atomic_store(&monitor_stop, false);
    pthread_t monitor;
    pthread_create(&monitor, NULL, tput_monitor, NULL);

    for (int i = 0; i < num_threads; i++)
    {
        args[i].t = t;

        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    for (int i = 0; i < num_threads; i++)
    {
        pthread_join(threads[i], NULL);
    }
    atomic_store(&monitor_stop, true);
    pthread_join(monitor, NULL);
    printf("\n");

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed =
        (end.tv_sec - start.tv_sec) +
        (end.tv_nsec - start.tv_nsec) / 1e9;

    double iops = total_keys / elapsed;

    printf("\nThreads: %d\n", num_threads);
    printf("Elapsed time: %.6f seconds\n", elapsed);
    printf("Average throughput: %.2f ops/sec\n", iops);

    /* ── Integrity verification (opt-in via VERIFY=1) ── */
    {
        const char *ve = getenv("VERIFY");
        if (ve && atoi(ve) != 0)
        {
            atomic_store(&verify_cursor, 0);
            atomic_store(&found_cnt, 0);
            atomic_store(&missing_cnt, 0);
            struct timespec vstart, vend;
            clock_gettime(CLOCK_MONOTONIC, &vstart);
            for (int i = 0; i < num_threads; i++)
                pthread_create(&threads[i], NULL, verifier, &args[i]);
            for (int i = 0; i < num_threads; i++)
                pthread_join(threads[i], NULL);
            clock_gettime(CLOCK_MONOTONIC, &vend);
            double velapsed =
                (vend.tv_sec - vstart.tv_sec) + (vend.tv_nsec - vstart.tv_nsec) / 1e9;
            int found = atomic_load(&found_cnt);
            int missing = atomic_load(&missing_cnt);
            printf("Verify: found %d / %d (%.2f%%)  missing %d  in %.2f s\n",
                   found, total_keys, 100.0 * found / total_keys, missing, velapsed);
        }
        else
        {
            printf("Verify: skipped (set VERIFY=1 to enable)\n");
        }
    }

    struct timespec close_start, close_end;
    clock_gettime(CLOCK_MONOTONIC, &close_start);
    cow_close(t);
    clock_gettime(CLOCK_MONOTONIC, &close_end);
    double close_elapsed =
        (close_end.tv_sec - close_start.tv_sec) +
        (close_end.tv_nsec - close_start.tv_nsec) / 1e9;
    printf("Elapsed (close): %.6f seconds\n", close_elapsed);
    printf("Elapsed (insert+close): %.6f seconds\n", elapsed + close_elapsed);

    free(threads);
    free(args);
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        printf("Usage: %s <key_num> <mode> <device>\n", argv[0]);
        printf("mode: 0=all  1 2 4 8 16 32 64\n");
        return 1;
    }

    total_keys = atoi(argv[1]);
    int mode = atoi(argv[2]);
    const char *dev_path = argv[3];

    all_keys = malloc(sizeof(*all_keys) * (size_t)total_keys);
    if (!all_keys)
    {
        perror("malloc keys failed");
        return 1;
    }

    for (int i = 0; i < total_keys; i++)
    {
        all_keys[i] = i;
    }

    /* BENCH_SEQUENTIAL=1 skips the Fisher-Yates shuffle so threads insert
     * keys in ascending order — used for the sequential-insert comparison
     * across variants.  Default (unset / 0) keeps the original randomised
     * order so existing bench numbers remain comparable. */
    const char *seq_env = getenv("BENCH_SEQUENTIAL");
    int sequential = seq_env && seq_env[0] == '1';

    if (!sequential)
    {
        srand(54321);
        for (int i = total_keys - 1; i > 0; i--)
        {
            int j = rand() % (i + 1);
            int tmp = all_keys[i];
            all_keys[i] = all_keys[j];
            all_keys[j] = tmp;
        }
    }

    printf("%s key set generated (%d keys).\n",
           sequential ? "Sequential" : "Random", total_keys);

    int thread_counts[] = {1, 2, 4, 8, 16, 32, 64};
    int num_configs = (int)(sizeof(thread_counts) / sizeof(thread_counts[0]));

    if (mode == 0)
    {
        for (int i = 0; i < num_configs; i++)
        {
            run_test(dev_path, thread_counts[i]);
        }
    }
    else
    {
        int valid = 0;
        for (int i = 0; i < num_configs; i++)
        {
            if (mode == thread_counts[i])
            {
                run_test(dev_path, mode);
                valid = 1;
                break;
            }
        }

        if (!valid)
        {
            printf("Invalid mode: %d\n", mode);
            printf("Allowed values: 0, 1, 2, 4, 8, 16, 32, 64\n");
            free(all_keys);
            return 1;
        }
    }

    free(all_keys);
    printf("\nAll tests completed.\n");
    return 0;
}
