#pragma once

#include <stddef.h>

struct DList {
  DList* prev = NULL;
  DList* next = NULL;
};

inline void dlist_init(DList* node) { node->prev = node->next = node; }

inline void dlist_insert_before(DList* target, DList* node) {
  DList* prev = target->prev;
  prev->next = node;
  node->prev = prev;
  node->next = target;
  target->prev = node;
}

inline void dlist_detach(DList* node) {
  DList* prev = node->prev;
  DList* next = node->next;
  prev->next = next;
  next->prev = prev;
}

inline bool dlist_empty(DList* node) { return node->next == node; }
