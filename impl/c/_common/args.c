#include "args.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static const char *const NAMES[OPT_COUNT] = {"mode", "input", "threads", "repeat", "warmup", "queries", "ops",
                                              "preload", "seed", "partitions", "batch", "queue", "latencies", "dump"};

// The slot of option name, or -1 if it is not a known option.
static int slot_of(const char *name) {
    for (int i = 0; i < OPT_COUNT; i++) if (strcmp(NAMES[i], name) == 0) return i;
    return -1;
}

// True if s is not empty and made of decimal digits only.
static int all_digits(const char *s) {
    if (*s == '\0') return 0;
    for (; *s != '\0'; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

// The value of a required option; ends the process if it is missing.
static const char *required(const Args *a, int slot) {
    if (a->values[slot] == NULL) {
        fatal("missing required option --%s for mode %s", NAMES[slot], a->values[OPT_MODE] == NULL ? "" : a->values[OPT_MODE]);
    }
    return a->values[slot];
}

// Ends the process if an option that does not belong to the mode is present.
static void forbidden(const Args *a, int slot) {
    if (a->values[slot] != NULL) fatal("option --%s does not apply to mode %s", NAMES[slot], a->values[OPT_MODE]);
}

// Reads the required option as an integer >= min that fits in an int32.
static int integer(const Args *a, int slot, int min) {
    const char *v = required(a, slot);
    if (all_digits(v) && strlen(v) <= 10) {
        long long n = atoll(v);
        if (n <= 2147483647LL && n >= min) return (int)n;
    }
    fatal("option --%s must be an integer >= %d, got '%s'", NAMES[slot], min, v);
}

int args_insert_share(const char *mode) {
    if (strcmp(mode, "build") == 0) return 100;
    if (strcmp(mode, "query") == 0) return 0;
    // mixed-<query share>-<insert share>
    if (strncmp(mode, "mixed-", 6) != 0) return -1;
    const char *dash = strrchr(mode, '-');
    size_t first_length = (size_t)(dash - (mode + 6));
    if (dash <= mode + 6 || first_length == 0) return -1;
    char first[16];
    if (first_length >= sizeof first) return -1;
    memcpy(first, mode + 6, first_length);
    first[first_length] = '\0';
    const char *second = dash + 1;
    if (!all_digits(first) || !all_digits(second) || strlen(second) > 9) return -1;
    int share = atoi(second);
    return share <= 100 ? share : -1;
}

int args_has(const Args *a, const char *name) {
    int slot = slot_of(name);
    return slot >= 0 && a->values[slot] != NULL;
}

Args args_parse(int argc, char **argv) {
    Args a;
    memset(&a, 0, sizeof a);
    for (int i = 1; i < argc; i += 2) {
        if (strncmp(argv[i], "--", 2) != 0) fatal("expected an option, got '%s'", argv[i]);
        int slot = slot_of(argv[i] + 2);
        if (slot < 0) fatal("unknown option %s", argv[i]);
        if (i + 1 >= argc) fatal("option %s has no value", argv[i]);
        if (a.values[slot] != NULL) fatal("option %s given twice", argv[i]);
        a.values[slot] = argv[i + 1];
    }
    a.mode = required(&a, OPT_MODE);
    int query = strcmp(a.mode, "query") == 0, mixed = strncmp(a.mode, "mixed-", 6) == 0;
    if (args_insert_share(a.mode) < 0) fatal("unknown mode '%s'", a.mode);
    a.input = required(&a, OPT_INPUT);
    a.threads = integer(&a, OPT_THREADS, 1);
    a.repeat = integer(&a, OPT_REPEAT, 1);
    a.warmup = integer(&a, OPT_WARMUP, 0);
    // Options that only some modes use: required there, an error elsewhere.
    static const int MODE_ONLY[] = {OPT_QUERIES, OPT_OPS, OPT_SEED, OPT_LATENCIES};
    for (int i = 0; i < 4; i++) {
        if (query || mixed) required(&a, MODE_ONLY[i]); else forbidden(&a, MODE_ONLY[i]);
    }
    if (mixed) required(&a, OPT_PRELOAD); else forbidden(&a, OPT_PRELOAD);
    if (query || mixed) {
        a.queries = a.values[OPT_QUERIES];
        a.latencies = a.values[OPT_LATENCIES];
        a.ops = integer(&a, OPT_OPS, 0);
        const char *s = a.values[OPT_SEED];
        errno = 0;
        char *end = NULL;
        a.seed = strtoull(s, &end, 10);
        if (!all_digits(s) || errno != 0 || *end != '\0') fatal("option --seed must be an unsigned integer, got '%s'", s);
    }
    if (mixed) a.preload = integer(&a, OPT_PRELOAD, 0);
    a.dump = a.values[OPT_DUMP];
    return a;
}
