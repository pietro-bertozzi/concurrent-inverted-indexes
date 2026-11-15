// The pseudo-random generator shared by every implementation in every language
// (docs/design.md, section Pseudo-random generator). uint64_t is unsigned, so the
// arithmetic wraps and the shifts are logical.
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t state;
} SplitMix64;

// Starts a generator at the given seed.
static inline void splitmix64_init(SplitMix64 *g, uint64_t seed) { g->state = seed; }

// Returns the next 64-bit value of the sequence.
static inline uint64_t splitmix64_next(SplitMix64 *g) {
    g->state += 0x9E3779B97F4A7C15ULL;
    uint64_t z = g->state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Returns a random index below m (m > 0): next() mod m, unsigned.
static inline int splitmix64_below(SplitMix64 *g, int m) {
    return (int)(splitmix64_next(g) % (uint64_t)m);
}
