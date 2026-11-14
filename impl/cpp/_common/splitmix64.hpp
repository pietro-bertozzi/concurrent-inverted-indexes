// The pseudo-random generator shared by every implementation in every language
// (docs/design.md, section Pseudo-random generator). uint64_t is unsigned, so the
// arithmetic wraps and the shifts are logical.
#pragma once
#include <cstdint>

class SplitMix64 {
public:
    explicit SplitMix64(uint64_t seed) : state_(seed) {}

    // Returns the next 64-bit value of the sequence.
    uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ULL;
        uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // Returns a random index below m (m > 0): next() mod m, unsigned.
    int below(int m) { return static_cast<int>(next() % static_cast<uint64_t>(m)); }

private:
    uint64_t state_;
};
