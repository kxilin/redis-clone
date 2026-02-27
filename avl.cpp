#include "avl.h"

#include <assert.h>

#include <cstdint>

static uint32_t max(uint32_t lhs, uint32_t rhs) {
  return lhs < rhs ? rhs : lhs;
}

static void avl_update(AVLNode* node) {
  node->height = 1 + max(avl_height(node->left), avl_height(node->right));
  node->cnt = 1 + avl_cnt(node->left) + avl_cnt(node->right);
}

static AVLNode* rot_left(AVLNode* node) {
  AVLNode* parent = node->parent;
  AVLNode* new_node = node->right;
  AVLNode* inner = new_node->left;
  // node <-> inner
  node->right = inner;
  if (inner) {
    inner->parent = node;
  }
  // new_node -> parent
  new_node->parent =
      parent;  // parent may be null. the parent to child link is not updated
               // here, as the rotated node may be a root node without a parent,
               // and only the caller knows how to link a root node.
  // new_node <-> node
  new_node->left = node;
  node->parent = new_node;
  // auxillary data
  avl_update(node);
  avl_update(new_node);
  return new_node;
}

static AVLNode* rot_right(AVLNode* node) {
  AVLNode* parent = node->parent;
  AVLNode* new_node = node->left;
  AVLNode* inner = new_node->right;
  // node <-> inner
  node->left = inner;
  if (inner) {
    inner->parent = node;
  }
  // new_node -> parent
  new_node->parent =
      parent;  // parent may be null. the parent to child link is not updated
               // here, as the rotated node may be a root node without a parent,
               // and only the caller knows how to link a root node.
  // new_node <-> node
  new_node->right = node;
  node->parent = new_node;
  // auxillary data
  avl_update(node);
  avl_update(new_node);
  return new_node;
}

static AVLNode* avl_fix_left(AVLNode* node) {
  if (avl_height(node->left->left) < avl_height(node->left->right)) {
    node->left = rot_left(node->left);
  }
  return rot_right(node);
}

static AVLNode* avl_fix_right(AVLNode* node) {
  if (avl_height(node->right->right) < avl_height(node->right->left)) {
    node->right = rot_right(node->right);
  }
  return rot_left(node);
}

AVLNode* avl_fix(AVLNode* node) {
  while (true) {
    // "from" is the pointer that points TO this node from above.
    // By default we point it at our local "node" variable itself,
    // so if this turns out to be the root (no parent), writing to
    // *from just updates our local variable, which we return at the end.
    AVLNode** from = &node;

    AVLNode* parent = node->parent;
    if (parent) {
      // Not the root, so "from" should be the actual field in the parent
      // that points down to us. Now if we write *from = something, we
      // directly update the parent's left or right pointer.
      from = parent->left == node ? &parent->left : &parent->right;
    }

    // Recalculate this node's height based on its children's heights.
    // We do this bottom-up as we walk toward the root.
    avl_update(node);

    // Check if the AVL invariant is violated (height difference of 2)
    uint32_t l = avl_height(node->left);
    uint32_t r = avl_height(node->right);
    if (l == r + 2) {
      // Left subtree is too tall. Fix it with rotation(s).
      // We write the result directly into *from, which patches
      // whatever pointer was pointing at this node (parent's left/right,
      // or our local variable if root).
      *from = avl_fix_left(node);
    } else if (l + 2 == r) {
      // Right subtree is too tall.
      *from = avl_fix_right(node);
    }

    if (!parent) {
      // We've reached the root. *from is the new root node
      // (possibly changed by a rotation above), return it so
      // the caller can update their root pointer.
      return *from;
    }

    // Move up to the parent and repeat, because the height change
    // here may have caused an imbalance higher up in the tree.
    node = parent;
  }
}

static AVLNode* avl_del_easy(AVLNode* node) {
  // This function handles the simple case: node has at most 1 child.
  // We just bypass the node by connecting its parent directly to its child.
  assert(!node->left || !node->right);

  // Grab the one child (or NULL if leaf node)
  AVLNode* child = node->left ? node->left : node->right;
  AVLNode* parent = node->parent;

  // Tell the child who its new parent is (skipping over the deleted node)
  if (child) {
    child->parent = parent;
  }

  // If node was the root, the child simply becomes the new root
  if (!parent) {
    return child;
  }

  // Make the parent point to the child instead of the deleted node.
  // We use ** so we can update whichever side (left or right) the
  // deleted node was on, in one uniform write.
  AVLNode** from = parent->left == node ? &parent->left : &parent->right;
  *from = child;

  // The tree height may have changed, so walk up and fix any imbalances.
  // avl_fix returns the (possibly new) root.
  return avl_fix(parent);
}

AVLNode* avl_del(AVLNode* node) {
  // If node has 0 or 1 children, use the simple case directly
  if (!node->left || !node->right) {
    return avl_del_easy(node);
  }

  // Hard case: node has 2 children.
  // We can't just bypass it, so we find the in-order successor (smallest
  // node in the right subtree) to take its place.
  AVLNode* victim = node->right;
  while (victim->left) {
    victim = victim->left;  // go left as far as possible
  }

  // Detach the successor from its current position. The successor has no
  // left child (we just walked as far left as possible), so this is always
  // the easy case. This also rebalances the tree and returns the new root.
  AVLNode* root = avl_del_easy(victim);

  // Copy node's connections (left, right, parent) into victim's struct.
  *victim = *node;

  // The children still think their parent is the deleted node.
  // Update them to point to victim instead.
  if (victim->left) {
    victim->left->parent = victim;
  }
  if (victim->right) {
    victim->right->parent = victim;
  }

  // Now plug victim into wherever node was in the tree.
  // Default from to &root in case node was the root.
  AVLNode** from = &root;
  AVLNode* parent = node->parent;
  if (parent) {
    // Not the root, so update whichever side of the parent pointed to node
    from = parent->left == node ? &parent->left : &parent->right;
  }
  *from = victim;  // parent (or root) now points to victim instead of node

  return root;
}
