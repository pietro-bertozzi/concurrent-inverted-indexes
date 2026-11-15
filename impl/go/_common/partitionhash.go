package main

// The partition of a term (docs/design.md, section Concurrent variants): FNV-1a 64-bit over
// the term's UTF-8 bytes, then (hash >> 32) mod P. The same in every language, so each
// partition holds the same terms everywhere. A Go string is already UTF-8 bytes.

const (
	fnvOffsetBasis uint64 = 0xcbf29ce484222325
	fnvPrime       uint64 = 0x100000001b3
)

// FNV1a returns the FNV-1a 64-bit hash of the bytes of s.
func FNV1a(s string) uint64 {
	h := fnvOffsetBasis
	for i := 0; i < len(s); i++ {
		h = (h ^ uint64(s[i])) * fnvPrime
	}
	return h
}

// PartitionOf returns the partition of term among partitions: (hash >> 32) mod P.
func PartitionOf(term string, partitions int) int {
	return int((FNV1a(term) >> 32) % uint64(partitions))
}
