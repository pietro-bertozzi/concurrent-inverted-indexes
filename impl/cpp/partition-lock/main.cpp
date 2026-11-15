// C++ partition-lock variant: P sub-maps by term, one lock each; --batch postings per acquisition.
#include <limits>
#include <memory>

#include "partition_lock_index.hpp"
#include "runner.hpp"

int main(int argc, char** argv) {
    return runVariant(argc, argv, std::numeric_limits<int>::max(),
                      [](const Args& a) { return std::make_unique<PartitionLockIndex>(a.partitions, a.batch); },
                      {"partitions", "batch"});
}
