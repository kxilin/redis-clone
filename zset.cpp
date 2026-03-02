#include "zset.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "hashtable.h"

static ZNode* znode_new(const char* name, size_t len, double score) {
  ZNode* node = (ZNode*)malloc(sizeof(ZNode) + len);
  assert(node);  // do not do this in real production server.
  avl_init(&node->tree);
  node->hmap.next = NULL;
  node->hmap.hcode = str_hash((uint8_t*)name, len);
  node->score = score;
  node->len = len;
  memcpy(&node->name[0], name, len);
  return node;
}

static void znode_del(ZNode* node) { free(node); }

static bool zless(AVLNode* lhs, double score, const char* name, size_t len) {
  ZNode* zl = container_of(lhs, ZNode, tree);
  if (zl->score != score) {
    return zl->score < score;
  }
  int rv = memcmp(zl->name, name, zl->len < len ? zl->len : len);
  return (rv != 0) ? (rv < 0) : (zl->len < len);
}

static bool zless(double score, const char* name, size_t len, AVLNode* rhs) {
  ZNode* zr = container_of(rhs, ZNode, tree);
  if (score != zr->score) return score < zr->score;
  int rv = memcmp(name, zr->name, len < zr->len ? len : zr->len);
  return (rv != 0) ? (rv < 0) : (len < zr->len);
}

static bool zless(AVLNode* lhs, AVLNode* rhs) {
  ZNode* zr = container_of(rhs, ZNode, tree);
  return zless(lhs, zr->score, zr->name, zr->len);
}

static void tree_insert(ZSet* zset, ZNode* node) {
  AVLNode* parent = NULL;
  AVLNode** from = &zset->root;
  while (*from) {
    parent = *from;
    from = zless(&node->tree, parent) ? &parent->left : &parent->right;
  }
  *from = &node->tree;
  node->tree.parent = parent;
  zset->root = avl_fix(&node->tree);
}

static void zset_update(ZSet* zset, ZNode* node, double score) {
  if (node->score == score) {
    return;
  }
  zset->root = avl_del(&node->tree);
  avl_init(&node->tree);
  node->score = score;
  tree_insert(zset, node);
}

bool zset_insert(ZSet* zset, const char* name, size_t len, double score) {
  if (ZNode* node = zset_lookup(zset, name, len)) {
    zset_update(zset, node, score);
    return false;
  }
  ZNode* node = znode_new(name, len, score);
  hm_insert(&zset->hmap, &node->hmap);
  tree_insert(zset, node);
  return true;
}

struct HKey {
  HNode node;
  const char* name = NULL;
  size_t len = 0;
};

static bool hcmp(HNode* node, HNode* key) {
  ZNode* znode = container_of(node, ZNode, hmap);
  HKey* hkey = container_of(key, HKey, node);
  if (znode->len != hkey->len) {
    return false;
  }
  return 0 == memcmp(znode->name, hkey->name, znode->len);
}

ZNode* zset_lookup(ZSet* zset, const char* name, size_t len) {
  if (!zset->root) {
    return NULL;
  }
  HKey key;
  key.node.hcode = str_hash((uint8_t*)name, len);
  key.name = name;
  key.len = len;
  HNode* found = hm_lookup(&zset->hmap, &key.node, &hcmp);
  return found ? container_of(found, ZNode, hmap) : NULL;
}

void zset_delete(ZSet* zset, ZNode* node) {
  HKey key;
  key.node.hcode = node->hmap.hcode;
  key.name = node->name;
  key.len = node->len;
  HNode* found = hm_delete(&zset->hmap, &key.node, &hcmp);
  assert(found);
  zset->root = avl_del(&node->tree);
  znode_del(node);
}

ZNode* zset_seekge(ZSet* zset, double score, const char* name, size_t len) {
  AVLNode* found = NULL;
  for (AVLNode* node = zset->root; node;) {
    if (zless(node, score, name, len)) {
      node = node->right;
    } else {
      found = node;
      node = node->left;
    }
  }
  return found ? container_of(found, ZNode, tree) : NULL;
}

ZNode* zset_seekle(ZSet* zset, double score, const char* name, size_t len) {
  AVLNode* found = NULL;
  for (AVLNode* node = zset->root; node;) {
    if (!zless(score, name, len, node)) {  // node <= target
      found = node;
      node = node->right;
    } else {
      node = node->left;
    }
  }
  return found ? container_of(found, ZNode, tree) : NULL;
}

int64_t zset_count(ZSet* zset, double lo_score, const char* lo_name,
                   size_t lo_len, double hi_score, const char* hi_name,
                   size_t hi_len) {
  ZNode* lo = zset_seekge(zset, lo_score, lo_name, lo_len);
  ZNode* hi = zset_seekle(zset, hi_score, hi_name, hi_len);
  if (!lo || !hi) {
    return 0;
  }
  int64_t count = avl_rank(&hi->tree) - avl_rank(&lo->tree) + 1;
  return count < 0 ? 0 : count;
}

ZNode* znode_offset(ZNode* node, int64_t offset) {
  AVLNode* tnode = node ? avl_offset(&node->tree, offset) : NULL;
  return tnode ? container_of(tnode, ZNode, tree) : NULL;
}

static void tree_dispose(AVLNode* node) {
  if (!node) {
    return;
  }
  tree_dispose(node->left);
  tree_dispose(node->right);
  znode_del(container_of(node, ZNode, tree));
}

void zset_clear(ZSet* zset) {
  hm_clear(&zset->hmap);
  tree_dispose(zset->root);
  zset->root = NULL;
}
