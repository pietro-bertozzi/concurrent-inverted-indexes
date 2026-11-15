package main

import (
	"math"
	"os"
)

// Go partition-writers variant: P sub-maps by term, one owner goroutine each; postings travel in bounded queues.
func main() {
	Run(os.Args[1:], math.MaxInt32, func(a *Args) Index {
		return NewPartitionWritersIndex(a.Partitions, a.Batch, a.Queue, a.Threads)
	}, "partitions", "batch", "queue")
}
