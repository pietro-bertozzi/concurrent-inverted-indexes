#define _POSIX_C_SOURCE 200809L
#include "runner.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "args.h"
#include "dump.h"
#include "opplan.h"
#include "proc.h"
#include "queries.h"
#include "queryengine.h"
#include "util.h"

// The current time of the monotonic clock in ns.
static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// Mean cost in ns of one clock reading: 1,000,000 back-to-back readings (docs/design.md, Query latencies).
static int64_t clock_cost_ns(void) {
    const int n = 1000000;
    volatile int64_t sink = 0;   // the readings are consumed, so they cannot be removed
    int64_t start = now_ns();
    for (int i = 0; i < n; i++) sink += now_ns();
    return (now_ns() - start) / n;
}

// The documents of the input file: one string per line (document ID = position), all in one buffer.
typedef struct {
    char *buffer;
    char **line;
    size_t *length;
    int count;
} Docs;

// Reads the input file; ends the process if it cannot be read or has no documents.
static Docs load(const char *file) {
    FILE *in = fopen(file, "rb");
    if (in == NULL) fatal("cannot open %s", file);
    size_t capacity = 1 << 20, size = 0;
    char *buffer = xmalloc(capacity);
    for (;;) {
        if (size == capacity) { capacity *= 2; buffer = xrealloc(buffer, capacity); }
        size_t got = fread(buffer + size, 1, capacity - size, in);
        if (got == 0) break;
        size += got;
    }
    if (ferror(in)) fatal("error while reading %s", file);
    fclose(in);
    buffer = xrealloc(buffer, size + 1);
    buffer[size] = '\0';

    Docs d = {buffer, NULL, NULL, 0};
    size_t lines_capacity = 0, pos = 0;
    while (pos < size) {
        char *newline = memchr(buffer + pos, '\n', size - pos);
        size_t end = newline == NULL ? size : (size_t)(newline - buffer);
        size_t next = end + 1;
        if (end > pos && buffer[end - 1] == '\r') end--;
        if ((size_t)d.count == lines_capacity) {
            lines_capacity = lines_capacity == 0 ? 1024 : lines_capacity * 2;
            d.line = xrealloc(d.line, lines_capacity * sizeof(char *));
            d.length = xrealloc(d.length, lines_capacity * sizeof(size_t));
        }
        d.line[d.count] = buffer + pos;
        d.length[d.count] = end - pos;
        d.count++;
        pos = next;
    }
    if (d.count == 0) fatal("%s has no documents", file);
    return d;
}

// Frees the documents.
static void docs_free(Docs *d) {
    free(d->buffer);
    free(d->line);
    free(d->length);
}

// Appends the latencies of one repetition to the latency file: rep, warmup, latency_ns.
static void write_latencies(FILE *out, int rep, int warmup, const int64_t *latency, int count) {
    for (int i = 0; i < count; i++) fprintf(out, "%d\t%d\t%lld\n", rep, warmup, (long long)latency[i]);
}

// One repetition: load, index (build / preload), run, finish; prints its row.
static void repetition(const Args *a, int rep, int warmup, int last, const Queries *queries,
                       Index *(*factory)(void), FILE *latencies) {
    int build = strcmp(a->mode, "build") == 0, mixed = strncmp(a->mode, "mixed-", 6) == 0;
    int share = args_insert_share(a->mode);
    ProcSnapshot before = proc_snapshot();

    int64_t t = now_ns();
    Docs docs = load(a->input);
    int64_t load_ns = now_ns() - t;

    // The plan (the operation order) is made before timing starts.
    OpPlan plan = {NULL, 0, 0, 0, 0};
    if (!build) {
        int inserts = opplan_total_inserts(a->ops, share);
        if ((int64_t)a->preload + inserts > docs.count) {
            fatal("mixed run needs %lld documents (preload %d + %d inserts) but %s has %d",
                  (long long)a->preload + inserts, a->preload, inserts, a->input, docs.count);
        }
        plan = opplan_make(a->ops, share, a->threads, 0, a->seed, queries->count);   // worker 0 (the only one)
    }

    Index *index = factory();
    int64_t preload_ns = 0, build_ns = 0, run_ns = 0, finalize_ns = 0, result_total = 0;
    int64_t *latency = NULL;

    if (build || strcmp(a->mode, "query") == 0) {
        t = now_ns();
        for (int d = 0; d < docs.count; d++) index->add(index, d, docs.line[d], docs.length[d]);
        build_ns = now_ns() - t;
        if (!build) {                       // queries need the finished index
            t = now_ns();
            index->finish(index);
            finalize_ns = now_ns() - t;
        }
    } else {
        t = now_ns();
        for (int d = 0; d < a->preload; d++) index->add(index, d, docs.line[d], docs.length[d]);
        preload_ns = now_ns() - t;
    }
    if (build) {
        t = now_ns();
        index->finish(index);
        finalize_ns = now_ns() - t;
    }

    if (!build) {
        latency = xmalloc((size_t)plan.queries * sizeof(int64_t));
        QueryEngine engine;
        queryengine_init(&engine);
        atomic_int next_doc = a->preload;   // the shared insert counter
        int cursor = plan.query_offset;
        int li = 0;
        t = now_ns();
        for (int i = 0; i < plan.count; i++) {
            if (plan.types[i] == OP_QUERY) {
                int64_t start = now_ns();
                result_total += queryengine_run(&engine, index, queries, cursor);
                latency[li++] = now_ns() - start;
                if (++cursor == queries->count) cursor = 0;
            } else {
                int d = atomic_fetch_add(&next_doc, 1);
                index->add(index, d, docs.line[d], docs.length[d]);
            }
        }
        run_ns = now_ns() - t;
        if (mixed) {
            t = now_ns();
            index->finish(index);
            finalize_ns = now_ns() - t;
        }
        fprintf(stderr, "rep %d: sum of query result sizes = %lld\n", rep, (long long)result_total);
        queryengine_destroy(&engine);
    }

    ProcSnapshot used = proc_minus(proc_snapshot(), before);
    int64_t peak = proc_peak_rss_bytes();
    int w = warmup ? 1 : 0;
    char row[512];
    snprintf(row, sizeof row, "%d\t%d\t%lld\t%lld\t%lld\t%lld\t%lld\t%d\t%d\t%d\t%d\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld",
             rep, w, (long long)load_ns, (long long)preload_ns, (long long)build_ns, (long long)run_ns,
             (long long)finalize_ns, build ? 0 : plan.queries, build ? 0 : plan.inserts, index->docs(index),
             index->terms(index), (long long)index->postings(index), (long long)peak, (long long)used.user_ns,
             (long long)used.sys_ns, (long long)used.ctx_vol, (long long)used.ctx_invol);

    // After the timers have stopped: the latencies, the row, the dump.
    if (!build) write_latencies(latencies, rep, w, latency, plan.queries);
    puts(row);
    fflush(stdout);
    if (a->dump != NULL && last) dump_write(index, a->dump);

    // Nothing of this repetition stays reachable, so repetitions do not accumulate memory.
    index->destroy(index);
    free(latency);
    opplan_free(&plan);
    docs_free(&docs);
}

// True if name is one of the uses.
static int in_uses(const char *name, const char *const *uses, int uses_count) {
    for (int i = 0; i < uses_count; i++) if (strcmp(uses[i], name) == 0) return 1;
    return 0;
}

// Runs all repetitions of a.
static void execute(const Args *a, int max_threads, Index *(*factory)(void), const char *const *uses, int uses_count) {
    if (a->threads > max_threads) {
        fatal("this variant supports at most %d worker(s), got --threads %d", max_threads, a->threads);
    }
    for (int i = 0; i < 3; i++) {
        static const char *const NAMES[] = {"partitions", "batch", "queue"};
        if (args_has(a, NAMES[i]) && !in_uses(NAMES[i], uses, uses_count)) {
            fatal("option --%s is not used by this variant", NAMES[i]);
        }
    }
    int build = strcmp(a->mode, "build") == 0;
    Queries queries;
    memset(&queries, 0, sizeof queries);
    if (!build) queries = queries_load(a->queries);
    fprintf(stderr, "c gcc %s, mode %s, %d worker(s), %d warm-up + %d repetitions\n", __VERSION__, a->mode,
            a->threads, a->warmup, a->repeat);
    fprintf(stderr, "clock reading cost: %lld ns (mean of 1000000 back-to-back readings)\n", (long long)clock_cost_ns());
    FILE *latencies = NULL;
    if (!build) {
        latencies = fopen(a->latencies, "wb");
        if (latencies == NULL) fatal("cannot write %s", a->latencies);
    }
    int total = a->warmup + a->repeat;
    for (int rep = 0; rep < total; rep++) {
        repetition(a, rep, rep < a->warmup, rep == total - 1, build ? NULL : &queries, factory, latencies);
    }
    if (!build) {
        if (fclose(latencies) != 0) fatal("error while writing %s", a->latencies);
        queries_free(&queries);
    }
}

int run_variant(int argc, char **argv, int max_threads, Index *(*factory)(void),
                const char *const *uses, int uses_count) {
    Args a = args_parse(argc, argv);
    execute(&a, max_threads, factory, uses, uses_count);
    return 0;
}
