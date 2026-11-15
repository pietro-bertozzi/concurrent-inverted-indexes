// C sequential variant: one worker thread, one hand-written hash map, no locks.
#include "runner.h"
#include "sequential_index.h"

int main(int argc, char **argv) { return run_variant(argc, argv, 1, sequential_index_new, NULL, 0); }
