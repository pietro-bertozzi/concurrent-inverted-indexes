// C++ sequential variant: one worker, one std::unordered_map, no locks.
#include <memory>

#include "runner.hpp"
#include "sequential_index.hpp"

int main(int argc, char** argv) {
    return runVariant(argc, argv, 1, [] { return std::make_unique<SequentialIndex>(); }, {});
}
