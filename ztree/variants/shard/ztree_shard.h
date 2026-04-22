#pragma once

#include "ztree_main.h"

#define ZTREE_NUM_SHARDS 13

typedef struct {
    ztree_t *primary;                    /* shard 0: full cow_open */
    ztree_t *shards[ZTREE_NUM_SHARDS];   /* all shards (shards[0] == primary) */
} shard_tree_t;

shard_tree_t *shard_open(const char *dev_path);
void          shard_insert(shard_tree_t *st, int64_t key, const char *value);
void          shard_close(shard_tree_t *st);
