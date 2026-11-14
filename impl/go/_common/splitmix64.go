package main

// SplitMix64 is the pseudo-random generator shared by every implementation in every language
// (docs/design.md, section Pseudo-random generator). Go's uint64 is unsigned, so the
// arithmetic wraps and the shifts are logical without any special care.
type SplitMix64 struct {
	state uint64
}

// NewSplitMix64 returns a generator that starts from state seed.
func NewSplitMix64(seed uint64) *SplitMix64 { return &SplitMix64{state: seed} }

// Next returns the next 64-bit value of the sequence.
func (r *SplitMix64) Next() uint64 {
	r.state += 0x9E3779B97F4A7C15
	z := r.state
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
	z = (z ^ (z >> 27)) * 0x94D049BB133111EB
	return z ^ (z >> 31)
}

// Below returns a random index below m (m > 0): Next() mod m, unsigned.
func (r *SplitMix64) Below(m int) int { return int(r.Next() % uint64(m)) }
