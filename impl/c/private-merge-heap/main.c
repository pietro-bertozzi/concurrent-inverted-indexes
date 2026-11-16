// C private-merge-heap variant: private-merge with a binary min-heap in the N-way merge.
#include <limits.h>

#include "private_merge_index.h"
#include "runner.h"

int main(int argc, char **argv) { return run_variant(argc, argv, INT_MAX, private_merge_heap_index_new, NULL, 0); }
