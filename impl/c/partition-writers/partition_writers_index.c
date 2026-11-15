#define _POSIX_C_SOURCE 200809L
#include "partition_writers_index.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "docpostings.h"
#include "hashmap.h"
#include "partitionhash.h"
#include "postingbuffer.h"
#include "queryengine.h"
#include "util.h"

typedef struct PartitionWritersIndex PartitionWritersIndex;

// One partition: its sub-map and its posting count. Only its owner writes them.
typedef struct {
    TermMap map;
    int64_t postings;
} Partition;

// A message: a full buffer of postings, all for one partition. The owner frees the buffer's arrays.
typedef struct {
    int partition;
    PostingBuffer postings;
} Message;

// A worker's mailbox: a bounded ring buffer of messages protected by a mutex. Any worker may
// push, only the owner pops; both operations are non-blocking (they say whether they succeeded).
typedef struct {
    pthread_mutex_t mutex;
    Message *slots;   // capacity entries
    int capacity;
    int head;         // index of the oldest message
    int count;
} Queue;

// One worker's private state: tokenizing buffer, grouping arrays and query scratch space.
typedef struct {
    Worker base;       // must be first
    PartitionWritersIndex *index;
    DocPostings doc;
    QueryEngine engine;
    int number;        // this worker
    int start;         // the partition this worker visits first (worker number mod P)
    long phases;       // flush calls made so far
    PostingBuffer *buffers;   // postings waiting for each partition (partitions entries)
    int *first;        // first[p] .. first[p+1]: slots of partition p in order (partitions + 1 entries)
    int *next;         // fill positions while grouping (partitions entries)
    int *part_of;      // partition of each posting of the document
    int *order;        // posting numbers, grouped by partition
    int capacity;      // size of part_of and order
} Handle;

struct PartitionWritersIndex {
    Index base;        // must be first: the harness sees only this part
    Partition *parts;
    int partitions;
    int batch;
    int workers;
    Queue *queues;     // queues[w]: the mailbox of worker w
    atomic_long finished;   // flush calls that have sent everything, over all phases
    atomic_int docs;
    Handle **handles;  // the handles made so far, freed in destroy
    int handle_count;
};

// Moves *m into the queue and returns 1, or returns 0 (and leaves *m alone) if the queue is full.
static int queue_try_push(Queue *q, const Message *m) {
    pthread_mutex_lock(&q->mutex);
    if (q->count == q->capacity) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }
    q->slots[(q->head + q->count) % q->capacity] = *m;
    q->count++;
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

// Takes the oldest message into *m and returns 1, or returns 0 if the queue is empty.
static int queue_try_pop(Queue *q, Message *m) {
    pthread_mutex_lock(&q->mutex);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }
    *m = q->slots[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

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

// Inserts the postings of *b into partition p; only the owner of p calls it.
static void apply(PartitionWritersIndex *x, int p, const PostingBuffer *b) {
    Partition *part = &x->parts[p];
    for (int k = 0; k < b->size; k++) {
        int created;
        PostingList *list = map_get_or_create(&part->map, b->terms[k], b->lengths[k], &created);
        postings_insert(list, b->docs[k], b->freqs[k]);
    }
    part->postings += b->size;
}

// An empty buffer with room for min(batch, 1024) postings.
static void new_buffer(const PartitionWritersIndex *x, PostingBuffer *b) {
    postingbuffer_init_with(b, x->batch < 1024 ? x->batch : 1024);
}

// Applies every message in our own queue (and frees its buffer); returns 1 if there was any.
static int drain(Handle *h) {
    PartitionWritersIndex *x = h->index;
    int any = 0;
    Message m;
    while (queue_try_pop(&x->queues[h->number], &m)) {
        apply(x, m.partition, &m.postings);
        postingbuffer_destroy(&m.postings);
        any = 1;
    }
    return any;
}

// Hands the buffer of partition p to its owner: applied directly if that is us, else queued.
static void send(Handle *h, int p) {
    PartitionWritersIndex *x = h->index;
    int owner = p % x->workers;
    if (owner == h->number) {
        apply(x, p, &h->buffers[p]);
        postingbuffer_clear(&h->buffers[p]);
        return;
    }
    Message m = {p, h->buffers[p]};   // the full buffer travels; no copy of the postings
    new_buffer(x, &h->buffers[p]);
    while (!queue_try_push(&x->queues[owner], &m)) {   // queue full: backpressure
        if (!drain(h)) sched_yield();                  // deadlock rule: empty our own queue before waiting
    }
}

static void add(Worker *self, int doc_id, const char *text, size_t length) {
    Handle *h = (Handle *)self;
    PartitionWritersIndex *x = h->index;
    docpostings_read(&h->doc, text, length);   // private work
    group(h);
    for (int i = 0; i < x->partitions; i++) {   // partitions from h->start, wrapping around
        int p = (h->start + i) % x->partitions;
        for (int s = h->first[p]; s < h->first[p + 1]; s++) {
            const DocTerm *t = &h->doc.terms[h->order[s]];
            postingbuffer_add(&h->buffers[p], t->term, t->length, doc_id, t->freq);
            if (h->buffers[p].size == x->batch) send(h, p);   // full: one message of `batch` postings
        }
    }
    atomic_fetch_add(&x->docs, 1);
    drain(h);   // regularly empty our own mailbox
}

// Ends the phase. First send everything still buffered. Then wait until every worker has done
// the same, emptying our own queue meanwhile: once all have sent, nothing new can arrive, and one
// last drain applies what is left. Every worker calls flush once per phase.
static void flush(Worker *self) {
    Handle *h = (Handle *)self;
    PartitionWritersIndex *x = h->index;
    for (int i = 0; i < x->partitions; i++) {
        int p = (h->start + i) % x->partitions;
        if (h->buffers[p].size > 0) send(h, p);
    }
    h->phases++;
    atomic_fetch_add(&x->finished, 1);
    while (atomic_load(&x->finished) < h->phases * x->workers) {
        if (!drain(h)) sched_yield();   // nothing to apply: let others run
    }
    drain(h);   // every message was queued before its sender's increment
}

static int query(Worker *self, const Queries *q, int i) {
    Handle *h = (Handle *)self;
    return queryengine_run(&h->engine, &h->index->base, q, i);   // nobody writes in query mode: no lock
}

static Worker *worker(Index *self, int number) {
    PartitionWritersIndex *x = (PartitionWritersIndex *)self;
    Handle *h = xcalloc(1, sizeof(Handle));
    h->base.add = add;
    h->base.query = query;
    h->base.flush = flush;
    h->index = x;
    h->number = number;
    h->start = number % x->partitions;
    docpostings_init(&h->doc);
    queryengine_init(&h->engine);
    h->buffers = xmalloc((size_t)x->partitions * sizeof(PostingBuffer));
    for (int p = 0; p < x->partitions; p++) new_buffer(x, &h->buffers[p]);
    h->first = xcalloc((size_t)x->partitions + 1, sizeof(int));
    h->next = xcalloc((size_t)x->partitions, sizeof(int));
    x->handles = xrealloc(x->handles, (size_t)(x->handle_count + 1) * sizeof(Handle *));
    x->handles[x->handle_count++] = h;
    return &h->base;
}

static const PostingList *get(const Index *self, const char *term, size_t length) {
    const PartitionWritersIndex *x = (const PartitionWritersIndex *)self;
    return map_get(&x->parts[partition_of(term, length, x->partitions)].map, term, length);
}

static void finish(Index *self) { (void)self; /* lists are kept sorted on every insert */ }
static int docs(const Index *self) { return atomic_load(&((PartitionWritersIndex *)self)->docs); }

static int terms(const Index *self) {
    const PartitionWritersIndex *x = (const PartitionWritersIndex *)self;
    size_t n = 0;
    for (int p = 0; p < x->partitions; p++) n += x->parts[p].map.count;
    return (int)n;
}

static int64_t postings(const Index *self) {
    const PartitionWritersIndex *x = (const PartitionWritersIndex *)self;
    int64_t n = 0;
    for (int p = 0; p < x->partitions; p++) n += x->parts[p].postings;
    return n;
}

static void each(const Index *self, IndexVisitor visit, void *context) {
    const PartitionWritersIndex *x = (const PartitionWritersIndex *)self;
    for (int p = 0; p < x->partitions; p++) map_each(&x->parts[p].map, visit, context);
}

static void destroy(Index *self) {
    PartitionWritersIndex *x = (PartitionWritersIndex *)self;
    for (int k = 0; k < x->handle_count; k++) {
        Handle *h = x->handles[k];
        docpostings_destroy(&h->doc);
        queryengine_destroy(&h->engine);
        for (int p = 0; p < x->partitions; p++) postingbuffer_destroy(&h->buffers[p]);
        free(h->buffers);
        free(h->first);
        free(h->next);
        free(h->part_of);
        free(h->order);
        free(h);
    }
    free(x->handles);
    for (int w = 0; w < x->workers; w++) {
        Queue *q = &x->queues[w];
        for (int k = 0; k < q->count; k++) postingbuffer_destroy(&q->slots[(q->head + k) % q->capacity].postings);
        free(q->slots);
        pthread_mutex_destroy(&q->mutex);
    }
    free(x->queues);
    for (int p = 0; p < x->partitions; p++) map_destroy(&x->parts[p].map);
    free(x->parts);
    free(x);
}

Index *partition_writers_index_new(const Args *args) {
    PartitionWritersIndex *x = xcalloc(1, sizeof(PartitionWritersIndex));
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
    x->workers = args->threads;
    x->parts = xcalloc((size_t)x->partitions, sizeof(Partition));
    for (int p = 0; p < x->partitions; p++) map_init(&x->parts[p].map);
    x->queues = xcalloc((size_t)x->workers, sizeof(Queue));
    for (int w = 0; w < x->workers; w++) {
        x->queues[w].capacity = args->queue;
        x->queues[w].slots = xmalloc((size_t)args->queue * sizeof(Message));
        pthread_mutex_init(&x->queues[w].mutex, NULL);
    }
    atomic_init(&x->finished, 0);
    atomic_init(&x->docs, 0);
    return &x->base;
}
