// C++ partition-writers variant: P sub-maps by term, one owner thread each; postings travel in bounded queues.
#include <limits>
#include <memory>

#include "partition_writers_index.hpp"
#include "runner.hpp"

int main(int argc, char** argv) {
    return runVariant(argc, argv, std::numeric_limits<int>::max(),
                      [](const Args& a) {
                          return std::make_unique<PartitionWritersIndex>(a.partitions, a.batch, a.queue, a.threads);
                      },
                      {"partitions", "batch", "queue"});
}
