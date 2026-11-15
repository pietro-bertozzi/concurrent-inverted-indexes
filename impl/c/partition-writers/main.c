// C partition-writers variant: P sub-maps by term, one owner thread each; postings travel in bounded queues.
#include <limits.h>

#include "partition_writers_index.h"
#include "runner.h"

int main(int argc, char **argv) {
    static const char *const USES[] = {"partitions", "batch", "queue"};
    return run_variant(argc, argv, INT_MAX, partition_writers_index_new, USES, 3);
}
