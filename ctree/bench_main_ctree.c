#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
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

static void reset_device(const char *dev_path)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sudo nvme zns reset-zone -a %s", dev_path);
    int rc = system(cmd);
    if (rc == -1)
        perror("system(nvme reset-zone)");

    /* Discard CNS device to reset FTL state */
    rc = system("sudo blkdiscard /dev/nvme3n1");
    if (rc == -1)
        perror("system(blkdiscard nvme3n1)");
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

    for (int i = 0; i < num_threads; i++)
    {
        args[i].t = t;

        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    for (int i = 0; i < num_threads; i++)
    {
        pthread_join(threads[i], NULL);
    }
    printf("\n");

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed =
        (end.tv_sec - start.tv_sec) +
        (end.tv_nsec - start.tv_nsec) / 1e9;

    double iops = total_keys / elapsed;

    printf("\nThreads: %d\n", num_threads);
    printf("Elapsed time: %.6f seconds\n", elapsed);
    printf("Average throughput: %.2f ops/sec\n", iops);

    cow_close(t);

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

    srand(54321);
    for (int i = total_keys - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        int tmp = all_keys[i];
        all_keys[i] = all_keys[j];
        all_keys[j] = tmp;
    }

    printf("Random key set generated (%d keys).\n", total_keys);

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
