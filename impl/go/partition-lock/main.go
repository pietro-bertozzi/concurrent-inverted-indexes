package main

import (
	"math"
	"os"
)

// Go partition-lock variant: P sub-maps by term, one lock each; --batch postings per acquisition.
func main() {
	Run(os.Args[1:], math.MaxInt32, func(a *Args) Index { return NewPartitionLockIndex(a.Partitions, a.Batch) }, "partitions", "batch")
}
