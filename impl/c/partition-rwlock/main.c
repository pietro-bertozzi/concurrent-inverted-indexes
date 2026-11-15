// C partition-rwlock variant: P sub-maps by term, one read/write lock each; --batch postings per acquisition.
#include <limits.h>

#include "partition_rwlock_index.h"
#include "runner.h"

int main(int argc, char **argv) {
    static const char *const USES[] = {"partitions", "batch"};
    return run_variant(argc, argv, INT_MAX, partition_rwlock_index_new, USES, 2);
}
