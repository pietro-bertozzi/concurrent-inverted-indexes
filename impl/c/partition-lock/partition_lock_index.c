#define _POSIX_C_SOURCE 200809L
#include "partition_lock_index.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "docpostings.h"
#include "hashmap.h"
#include "partitionhash.h"
#include "queryengine.h"
#include "util.h"

typedef struct PartitionLockIndex PartitionLockIndex;

// One partition: its sub-map, its lock and its posting count (guarded by the lock).
typedef struct {
    TermMap map;
    pthread_mutex_t mutex;   // the default, non-fair mutex
    int64_t postings;
} Partition;

// One worker's private state: tokenizing buffer, grouping arrays and query scratch space.
typedef struct {
    Worker base;       // must be first
    PartitionLockIndex *index;
    DocPostings doc;
    QueryEngine engine;
    int *first;        // first[p] .. first[p+1]: slots of partition p in order (partitions + 1 entries)
    int *next;         // fill positions while grouping (partitions entries)
    int *part_of;      // partition of each posting of the document
    int *order;        // posting numbers, grouped by partition
    int capacity;      // size of part_of and order
} Handle;

struct PartitionLockIndex {
    Index base;        // must be first: the harness sees only this part
    Partition *parts;
    int partitions;
    int batch;
    atomic_int docs;
    Handle **handles;  // the handles made so far, freed in destroy
    int handle_count;
};

// Counting sort of the document's postings by partition: fills h->first and h->order.
static void group(Handle *h) {
    int n = h->doc.count, parts = h->index->partitions;
    if (h->capacity < n) {
        h->capacity = 2 * n;
        h->part_of = xrealloc(h->part_of, (size_t)h->capacity * sizeof(int));
        h->order = xrealloc(h->order, (size_t)h->capacity * sizeof(int));
    }
    for (int p = 0; p <= parts; p++) h->first[p] = 0;
    for (int k = 0; k < n; k++) {
        h->part_of[k] = partition_of_hash(h->doc.terms[k].hash, parts);
        h->first[h->part_of[k] + 1]++;
    }
    for (int p = 0; p < parts; p++) h->first[p + 1] += h->first[p];   // first[p] = start of partition p
    for (int p = 0; p < parts; p++) h->next[p] = h->first[p];
    for (int k = 0; k < n; k++) h->order[h->next[h->part_of[k]]++] = k;
}

static void add(Worker *self, int doc_id, const char *text, size_t length) {
    Handle *h = (Handle *)self;
    PartitionLockIndex *x = h->index;
    docpostings_read(&h->doc, text, length);   // no lock: private work
    group(h);
    for (int p = 0; p < x->partitions; p++) {
        Partition *part = &x->parts[p];
        int end = h->first[p + 1];
        for (int from = h->first[p]; from < end; from += x->batch) {
            int to = end - from > x->batch ? from + x->batch : end;
            pthread_mutex_lock(&part->mutex);
            for (int s = from; s < to; s++) {
                const DocTerm *t = &h->doc.terms[h->order[s]];
                int created;
                PostingList *list = map_get_or_create(&part->map, t->term, t->length, &created);
                postings_insert(list, doc_id, t->freq);
            }
            part->postings += to - from;
            pthread_mutex_unlock(&part->mutex);
            if (to == end) break;
        }
    }
    atomic_fetch_add(&x->docs, 1);
}

static int query(Worker *self, const Queries *q, int i) {
    Handle *h = (Handle *)self;
    PartitionLockIndex *x = h->index;
    int a = partition_of(q->first[i], q->first_length[i], x->partitions);
    int b = q->kind[i] == Q_SIMPLE ? a : partition_of(q->second[i], q->second_length[i], x->partitions);
    int low = a < b ? a : b, high = a < b ? b : a;   // fixed lock order: lower number first
    pthread_mutex_lock(&x->parts[low].mutex);
    if (high != low) pthread_mutex_lock(&x->parts[high].mutex);
    int size = queryengine_run(&h->engine, &x->base, q, i);
    if (high != low) pthread_mutex_unlock(&x->parts[high].mutex);
    pthread_mutex_unlock(&x->parts[low].mutex);
    return size;
}

static Worker *worker(Index *self) {
    PartitionLockIndex *x = (PartitionLockIndex *)self;
    Handle *h = xcalloc(1, sizeof(Handle));
    h->base.add = add;
    h->base.query = query;
    h->index = x;
    docpostings_init(&h->doc);
    queryengine_init(&h->engine);
    h->first = xcalloc((size_t)x->partitions + 1, sizeof(int));
    h->next = xcalloc((size_t)x->partitions, sizeof(int));
    x->handles = xrealloc(x->handles, (size_t)(x->handle_count + 1) * sizeof(Handle *));
    x->handles[x->handle_count++] = h;
    return &h->base;
}

static const PostingList *get(const Index *self, const char *term, size_t length) {
    const PartitionLockIndex *x = (const PartitionLockIndex *)self;
    return map_get(&x->parts[partition_of(term, length, x->partitions)].map, term, length);
}

static void finish(Index *self) { (void)self; /* lists are kept sorted on every insert */ }
static int docs(const Index *self) { return atomic_load(&((PartitionLockIndex *)self)->docs); }

static int terms(const Index *self) {
    const PartitionLockIndex *x = (const PartitionLockIndex *)self;
    size_t n = 0;
    for (int p = 0; p < x->partitions; p++) n += x->parts[p].map.count;
    return (int)n;
}

static int64_t postings(const Index *self) {
    const PartitionLockIndex *x = (const PartitionLockIndex *)self;
    int64_t n = 0;
    for (int p = 0; p < x->partitions; p++) n += x->parts[p].postings;
    return n;
}

static void each(const Index *self, IndexVisitor visit, void *context) {
    const PartitionLockIndex *x = (const PartitionLockIndex *)self;
    for (int p = 0; p < x->partitions; p++) map_each(&x->parts[p].map, visit, context);
}

static void destroy(Index *self) {
    PartitionLockIndex *x = (PartitionLockIndex *)self;
    for (int k = 0; k < x->handle_count; k++) {
        Handle *h = x->handles[k];
        docpostings_destroy(&h->doc);
        queryengine_destroy(&h->engine);
        free(h->first);
        free(h->next);
        free(h->part_of);
        free(h->order);
        free(h);
    }
    free(x->handles);
    for (int p = 0; p < x->partitions; p++) {
        map_destroy(&x->parts[p].map);
        pthread_mutex_destroy(&x->parts[p].mutex);
    }
    free(x->parts);
    free(x);
}

Index *partition_lock_index_new(const Args *args) {
    PartitionLockIndex *x = xcalloc(1, sizeof(PartitionLockIndex));
    x->base.worker = worker;
    x->base.get = get;
    x->base.finish = finish;
    x->base.docs = docs;
    x->base.terms = terms;
    x->base.postings = postings;
    x->base.each = each;
    x->base.destroy = destroy;
    x->partitions = args->partitions;
    x->batch = args->batch;
    x->parts = xcalloc((size_t)x->partitions, sizeof(Partition));
    for (int p = 0; p < x->partitions; p++) {
        map_init(&x->parts[p].map);
        pthread_mutex_init(&x->parts[p].mutex, NULL);
    }
    atomic_init(&x->docs, 0);
    return &x->base;
}
