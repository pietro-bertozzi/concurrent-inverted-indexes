// C private-merge variant: a private index per worker, merged in parallel at the end (finalize).
#include <limits.h>

#include "private_merge_index.h"
#include "runner.h"

int main(int argc, char **argv) { return run_variant(argc, argv, INT_MAX, private_merge_index_new, NULL, 0); }
