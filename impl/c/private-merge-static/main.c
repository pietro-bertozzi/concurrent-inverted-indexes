// C private-merge-static variant: private-merge with the documents split into N equal ranges up front.
#include <limits.h>

#include "private_merge_index.h"
#include "runner.h"

int main(int argc, char **argv) { return run_variant(argc, argv, INT_MAX, private_merge_static_index_new, NULL, 0); }
