// C++ private-merge-heap variant: private-merge with a binary min-heap in the N-way merge.
#include <limits>
#include <memory>

#include "private_merge_index.hpp"
#include "runner.hpp"

int main(int argc, char** argv) {
    return runVariant(argc, argv, std::numeric_limits<int>::max(),
                      [](const Args& a) { return std::make_unique<PrivateMergeIndex>(a.threads, true); }, {});
}
