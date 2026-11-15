// The term -> posting list hash map of the C implementations. C has no standard hash map, so
// this one is our own (docs/design.md, section Data structures): open addressing with linear
// probing, no deletion, FNV-1a 64-bit hash, power-of-two capacity doubled when the load factor
// would exceed 0.5.
//
// A slot holds the hash, an owned copy of the key, the key length and a pointer to a
// separately allocated posting list. The lists are therefore never moved when the table
// grows, so a pointer to a list stays valid (concurrent variants can later put a lock in
// each list).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "postings.h"

typedef struct {
    uint64_t hash;
    char *key;            // owned, NUL-terminated copy; NULL marks an empty slot
    uint32_t key_length;  // bytes, without the NUL
    PostingList *list;    // owned
} MapSlot;

typedef struct {
    MapSlot *slots;
    size_t capacity;      // power of two
    size_t count;         // number of keys
} TermMap;

// Creates an empty map.
void map_init(TermMap *map);

// Frees all keys, all posting lists and the table.
void map_destroy(TermMap *map);

// Returns the posting list of the key (bytes, length), or NULL if absent. The key is not
// copied; it need not be NUL-terminated.
PostingList *map_get(const TermMap *map, const char *key, size_t length);

// Returns the posting list of the key, creating an empty one (and copying the key) if it is
// absent; *created tells which happened.
PostingList *map_get_or_create(TermMap *map, const char *key, size_t length, int *created);

// Calls visit(context, key, length, list) for every entry, in table order.
void map_each(const TermMap *map,
              void (*visit)(void *context, const char *key, size_t length, const PostingList *list),
              void *context);
