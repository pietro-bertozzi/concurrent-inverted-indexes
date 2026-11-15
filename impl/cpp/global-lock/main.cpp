// C++ global-lock variant: one lock for the whole index; --batch postings per acquisition.
#include <limits>
#include <memory>

#include "global_lock_index.hpp"
#include "runner.hpp"

int main(int argc, char** argv) {
    return runVariant(argc, argv, std::numeric_limits<int>::max(),
                      [](const Args& a) { return std::make_unique<GlobalLockIndex>(a.batch); }, {"batch"});
}
