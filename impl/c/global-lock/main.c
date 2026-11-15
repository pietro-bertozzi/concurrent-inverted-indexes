// C global-lock variant: one lock for the whole index; --batch postings per acquisition.
#include <limits.h>

#include "global_lock_index.h"
#include "runner.h"

int main(int argc, char **argv) {
    static const char *const USES[] = {"batch"};
    return run_variant(argc, argv, INT_MAX, global_lock_index_new, USES, 1);
}
