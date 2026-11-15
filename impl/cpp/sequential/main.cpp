// C++ sequential variant: one worker thread, one std::unordered_map, no locks.
#include <memory>

#include "runner.hpp"
#include "sequential_index.hpp"

int main(int argc, char** argv) {
    return runVariant(argc, argv, 1, [](const Args&) { return std::make_unique<SequentialIndex>(); }, {});
}
