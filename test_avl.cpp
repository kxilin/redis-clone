#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <set>

#include "avl.h"

#define container_of(ptr, type, member)               \
  ({                                                  \
    const typeof(((type*)0)->member)* __mptr = (ptr); \
    (type*)((char*)__mptr - offsetof(type, member));  \
  })

// Data is the actual user struct. It embeds an AVLNode so the tree
// can manage it without knowing about the val field.
struct Data {
  AVLNode node;
  uint32_t val = 0;
};

// Container just holds the root pointer of the tree.
struct Container {
  AVLNode* root = NULL;
};

static void add(Container& c, uint32_t val) {
  Data* data = new Data();
  avl_init(&data->node);
  data->val = val;

  // Walk down the tree to find the correct insertion spot.
  // `from` tracks the pointer we need to update when we find the empty slot.
  // `cur` tracks the parent of the new node.
  AVLNode* cur = NULL;
  AVLNode** from = &c.root;
  while (*from) {
    cur = *from;
    uint32_t node_val = container_of(cur, Data, node)->val;
    from = (val < node_val) ? &cur->left : &cur->right;
  }
  // *from is now NULL — the empty slot where the new node belongs.
  // Writing here updates the parent's left/right pointer directly.
  *from = &data->node;
  data->node.parent = cur;
  // Walk back up to the root fixing any AVL imbalances caused by the insert.
  c.root = avl_fix(&data->node);
}

static bool del(Container& c, uint32_t val) {
  // Standard BST search for the node to delete.
  AVLNode* cur = c.root;
  while (cur) {
    uint32_t node_val = container_of(cur, Data, node)->val;
    if (val == node_val) {
      break;
    }
    cur = val < node_val ? cur->left : cur->right;
  }
  if (!cur) {
    return false;  // not found
  }

  // Detach from the AVL tree (handles rebalancing internally),
  // then free the Data struct that owns this node.
  c.root = avl_del(cur);
  delete container_of(cur, Data, node);
  return true;
}

// Recursively verifies every invariant of the AVL tree.
// Called after every operation in tests to catch bugs immediately.
static void avl_verify(AVLNode* parent, AVLNode* node) {
  if (!node) {
    return;
  }

  // Parent pointer must be correct.
  assert(node->parent == parent);
  avl_verify(node, node->left);
  avl_verify(node, node->right);

  // Node count must equal 1 (self) + left subtree count + right subtree count.
  assert(node->cnt == 1 + avl_cnt(node->left) + avl_cnt(node->right));

  // AVL invariant: left and right subtree heights can differ by at most 1.
  uint32_t l = avl_height(node->left);
  uint32_t r = avl_height(node->right);
  assert(l == r || l + 1 == r || l == r + 1);

  // Stored height must match the actual computed height.
  assert(node->height == 1 + std::max(l, r));

  // BST ordering: left child's value <= this node's value <= right child's value.
  uint32_t val = container_of(node, Data, node)->val;
  if (node->left) {
    assert(node->left->parent == node);
    assert(container_of(node->left, Data, node)->val <= val);
  }
  if (node->right) {
    assert(node->right->parent == node);
    assert(container_of(node->right, Data, node)->val >= val);
  }
}

// In-order traversal to collect all values into a multiset.
// Used to compare tree contents against the reference.
static void extract(AVLNode* node, std::multiset<uint32_t>& extracted) {
  if (!node) {
    return;
  }
  extract(node->left, extracted);
  extracted.insert(container_of(node, Data, node)->val);
  extract(node->right, extracted);
}

// Full verification: checks structure AND that the values match the reference.
static void container_verify(Container& c, const std::multiset<uint32_t>& ref) {
  avl_verify(NULL, c.root);
  assert(avl_cnt(c.root) == ref.size());
  std::multiset<uint32_t> extracted;
  extract(c.root, extracted);
  assert(extracted == ref);  // tree must contain exactly the same values as ref
}

// Free all nodes in the tree by repeatedly deleting the root.
static void dispose(Container& c) {
  while (c.root) {
    AVLNode* node = c.root;
    c.root = avl_del(c.root);
    delete container_of(node, Data, node);
  }
}

// Tests inserting val at every possible position in a tree of size sz.
// For each val: build tree with all values EXCEPT val, verify, then
// insert val and verify again. This covers inserting at the start,
// end, and every middle position.
static void test_insert(uint32_t sz) {
  for (uint32_t val = 0; val < sz; ++val) {
    Container c;
    std::multiset<uint32_t> ref;
    for (uint32_t i = 0; i < sz; ++i) {
      if (i == val) {
        continue;  // skip val so we can insert it last
      }
      add(c, i);
      ref.insert(i);
    }
    container_verify(c, ref);

    add(c, val);  // now insert the missing value
    ref.insert(val);
    container_verify(c, ref);
    dispose(c);
  }
}

// Tests inserting duplicates. For each val: build a full tree of 0..sz-1,
// then insert val again as a duplicate and verify the tree stays valid.
static void test_insert_dup(uint32_t sz) {
  for (uint32_t val = 0; val < sz; ++val) {
    Container c;
    std::multiset<uint32_t> ref;
    for (uint32_t i = 0; i < sz; ++i) {
      add(c, i);
      ref.insert(i);
    }
    container_verify(c, ref);

    add(c, val);  // insert duplicate
    ref.insert(val);
    container_verify(c, ref);
    dispose(c);
  }
}

// Tests deleting from every possible position in a tree of size sz.
// For each val: build full tree, verify, delete val, verify again.
static void test_remove(uint32_t sz) {
  for (uint32_t val = 0; val < sz; ++val) {
    Container c;
    std::multiset<uint32_t> ref;
    for (uint32_t i = 0; i < sz; ++i) {
      add(c, i);
      ref.insert(i);
    }
    container_verify(c, ref);

    assert(del(c, val));  // must succeed since val is in the tree
    ref.erase(val);
    container_verify(c, ref);
    dispose(c);
  }
}

int main() {
  Container c;

  printf("starting\n");

  // Sanity checks on a trivially small tree.
  container_verify(c, {});
  add(c, 123);
  container_verify(c, {123});
  assert(!del(c, 124));   // deleting nonexistent value returns false
  assert(del(c, 123));    // deleting existing value returns true
  container_verify(c, {});
  printf("quick done\n");

  // Insert 0, 3, 6, ..., 999 in order. Sequential inserts stress
  // the right-leaning imbalance case since values always go right.
  std::multiset<uint32_t> ref;
  for (uint32_t i = 0; i < 1000; i += 3) {
    add(c, i);
    ref.insert(i);
    container_verify(c, ref);
  }
  printf("sequential insertion done\n");

  // Random inserts on top of the existing tree.
  for (uint32_t i = 0; i < 100; i++) {
    uint32_t val = (uint32_t)rand() % 1000;
    add(c, val);
    ref.insert(val);
    container_verify(c, ref);
  }
  printf("random insertion done\n");

  // Random deletions. If the value isn't in the tree, del() must return false.
  for (uint32_t i = 0; i < 200; i++) {
    uint32_t val = (uint32_t)rand() % 1000;
    auto it = ref.find(val);
    if (it == ref.end()) {
      assert(!del(c, val));
    } else {
      assert(del(c, val));
      ref.erase(it);
    }
    container_verify(c, ref);
  }
  printf("random deletion done\n");

  // Exhaustive insert/delete tests for all tree sizes up to 200.
  // This is O(n^3) but catches corner cases at every size.
  for (uint32_t i = 0; i < 200; ++i) {
    printf("i=%u\n", i);
    test_insert(i);
    printf("test_insert done\n");
    test_insert_dup(i);
    printf("test_insert_dup done\n");
    test_remove(i);
    printf("test_remove done\n");
  }

  printf("done\n");

  dispose(c);
  return 0;
}
