// FNV-1a 64-bit and the partition of a term (docs/design.md, section Concurrent variants):
// (hash >> 32) mod P. The same in every language, so each partition holds the same terms
// everywhere. The high bits are used because the C hash map (hashmap.c) picks its slot from
// the low bits of the same hash; using those too would crowd each partition's terms into a
// fraction of the map's slots.
#pragma once
#include <stddef.h>
#include <stdint.h>

// FNV-1a, 64 bit: start at the offset basis, then for every byte xor it in and multiply by the prime.
static inline uint64_t fnv1a(const char *key, size_t length) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < length; i++) {
        h ^= (unsigned char)key[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// The partition of a term whose FNV-1a hash is `hash`, among `partitions`: (hash >> 32) mod P.
static inline int partition_of_hash(uint64_t hash, int partitions) {
    return (int)((hash >> 32) % (uint64_t)partitions);
}

// The partition of the term (bytes, length) among `partitions`.
static inline int partition_of(const char *term, size_t length, int partitions) {
    return partition_of_hash(fnv1a(term, length), partitions);
}
