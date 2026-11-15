// The partition of a term (docs/design.md, section Concurrent variants): FNV-1a 64-bit over
// the term's UTF-8 bytes, then (hash >> 32) mod P. The same in every language, so each
// partition holds the same terms everywhere. A std::string already holds the UTF-8 bytes.
#pragma once
#include <cstdint>
#include <string>

inline uint64_t fnv1a(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ULL;
    return h;
}

// The partition of term among partitions: (hash >> 32) mod P.
inline int partitionOf(const std::string& term, int partitions) {
    return static_cast<int>((fnv1a(term) >> 32) % static_cast<uint64_t>(partitions));
}
