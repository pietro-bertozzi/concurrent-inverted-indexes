#include "hashmap.h"

#include <stdlib.h>
#include <string.h>

#include "partitionhash.h"
#include "util.h"

#define INITIAL_CAPACITY 1024

void map_init(TermMap *map) {
    map->capacity = INITIAL_CAPACITY;
    map->count = 0;
    map->slots = xcalloc(map->capacity, sizeof(MapSlot));
}

void map_destroy(TermMap *map) {
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->slots[i].key != NULL) {
            free(map->slots[i].key);
            postings_free(map->slots[i].list);
        }
    }
    free(map->slots);
    map->slots = NULL;
    map->capacity = map->count = 0;
}

// Finds the slot of the key: the slot that holds it, or the empty slot where linear probing
// ends (where it would be inserted). The hash is compared before the bytes.
static MapSlot *find_slot(const TermMap *map, uint64_t hash, const char *key, size_t length) {
    size_t mask = map->capacity - 1;
    size_t i = (size_t)hash & mask;
    for (;;) {
        MapSlot *s = &map->slots[i];
        if (s->key == NULL) return s;
        if (s->hash == hash && s->key_length == length && memcmp(s->key, key, length) == 0) return s;
        i = (i + 1) & mask;
    }
}

// Doubles the table and moves every slot to its place in the new one (the stored hash is
// reused, keys are not hashed again; keys and posting lists themselves do not move).
static void grow(TermMap *map) {
    MapSlot *old = map->slots;
    size_t old_capacity = map->capacity;
    map->capacity = old_capacity * 2;
    map->slots = xcalloc(map->capacity, sizeof(MapSlot));
    size_t mask = map->capacity - 1;
    for (size_t i = 0; i < old_capacity; i++) {
        if (old[i].key == NULL) continue;
        size_t j = (size_t)old[i].hash & mask;
        while (map->slots[j].key != NULL) j = (j + 1) & mask;
        map->slots[j] = old[i];
    }
    free(old);
}

PostingList *map_get(const TermMap *map, const char *key, size_t length) {
    MapSlot *s = find_slot(map, fnv1a(key, length), key, length);
    return s->key == NULL ? NULL : s->list;
}

PostingList *map_get_or_create(TermMap *map, const char *key, size_t length, int *created) {
    uint64_t hash = fnv1a(key, length);
    MapSlot *s = find_slot(map, hash, key, length);
    if (s->key != NULL) {
        *created = 0;
        return s->list;
    }
    if ((map->count + 1) * 2 > map->capacity) {   // load factor would exceed 0.5
        grow(map);
        s = find_slot(map, hash, key, length);
    }
    s->hash = hash;
    s->key = xmalloc(length + 1);
    memcpy(s->key, key, length);
    s->key[length] = '\0';
    s->key_length = (uint32_t)length;
    s->list = postings_new();
    map->count++;
    *created = 1;
    return s->list;
}

void map_each(const TermMap *map,
              void (*visit)(void *context, const char *key, size_t length, const PostingList *list),
              void *context) {
    for (size_t i = 0; i < map->capacity; i++) {
        const MapSlot *s = &map->slots[i];
        if (s->key != NULL) visit(context, s->key, s->key_length, s->list);
    }
}
