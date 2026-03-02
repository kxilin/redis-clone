#pragma once

#include "avl.h"
#include "hashtable.h"

struct ZSet {
  AVLNode* root = NULL;  // index by (score, name) using AVL tree
  HMap hmap;             // indey by name using hashmap
};

struct ZNode {
  AVLNode tree;
  HNode hmap;
  double score = 0;
  size_t len = 0;
  char name[0];  // flexible size array at end of struct
};

bool zset_insert(ZSet* zset, const char* name, size_t len, double score);
ZNode* zset_lookup(ZSet* zset, const char* name, size_t len);
void zset_delete(ZSet* zset, ZNode* node);
ZNode* zset_seekge(ZSet* zset, double score, const char* name, size_t len);
ZNode* zset_seekle(ZSet* zset, double score, const char* name, size_t len);
int64_t zset_count(ZSet* zset, double lo_score, const char* lo_name,
                   size_t lo_len, double hi_score, const char* hi_name,
                   size_t hi_len);
void zset_clear(ZSet* zset);
ZNode* znode_offset(ZNode* node, int64_t offset);
