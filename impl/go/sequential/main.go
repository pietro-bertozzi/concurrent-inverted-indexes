package main

import "os"

// Go sequential variant: one worker goroutine, one built-in map, no locks.
func main() {
	Run(os.Args[1:], 1, func(a *Args) Index { return NewSequentialIndex() })
}
