#define _POSIX_C_SOURCE 200809L
#include "global_lock_index.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "docpostings.h"
#include "hashmap.h"
#include "postingbuffer.h"
#include "queryengine.h"
#include "util.h"

typedef struct GlobalLockIndex GlobalLockIndex;

// One worker's private state: its tokenizing buffer and its query scratch space.
typedef struct {
    Worker base;       // must be first
    GlobalLockIndex *index;
    DocPostings doc;
    PostingBuffer buffer;
    QueryEngine engine;
} Handle;

struct GlobalLockIndex {
    Index base;        // must be first: the harness sees only this part
    TermMap map;
    pthread_mutex_t mutex;   // the default, non-fair mutex
    int batch;
    int64_t postings;        // guarded by mutex
    atomic_int docs;
    Handle **handles;        // the handles made so far, freed in destroy
    int handle_count;
};

// Inserts the whole buffer of h under one acquisition of the lock.
static void flush_buffer(Handle *h) {
    GlobalLockIndex *x = h->index;
    PostingBuffer *b = &h->buffer;
    pthread_mutex_lock(&x->mutex);
    for (int k = 0; k < b->size; k++) {
        int created;
        PostingList *list = map_get_or_create(&x->map, b->terms[k], b->lengths[k], &created);
        postings_insert(list, b->docs[k], b->freqs[k]);
    }
    x->postings += b->size;
    pthread_mutex_unlock(&x->mutex);
    postingbuffer_clear(b);
}

static void add(Worker *self, int doc_id, const char *text, size_t length) {
    Handle *h = (Handle *)self;
    docpostings_read(&h->doc, text, length);   // no lock: private work
    for (int k = 0; k < h->doc.count; k++) {
        const DocTerm *t = &h->doc.terms[k];
        postingbuffer_add(&h->buffer, t->term, t->length, doc_id, t->freq);
        if (h->buffer.size == h->index->batch) flush_buffer(h);   // full: one acquisition for `batch` postings
    }
    atomic_fetch_add(&h->index->docs, 1);
}

static void flush(Worker *self) {
    Handle *h = (Handle *)self;
    if (h->buffer.size > 0) flush_buffer(h);
}

static int query(Worker *self, const Queries *q, int i) {
    Handle *h = (Handle *)self;
    pthread_mutex_lock(&h->index->mutex);
    int size = queryengine_run(&h->engine, &h->index->base, q, i);
    pthread_mutex_unlock(&h->index->mutex);
    return size;
}

static Worker *worker(Index *self, int number) {
    (void)number;
    GlobalLockIndex *x = (GlobalLockIndex *)self;
    Handle *h = xcalloc(1, sizeof(Handle));
    h->base.add = add;
    h->base.query = query;
    h->base.flush = flush;
    h->index = x;
    docpostings_init(&h->doc);
    postingbuffer_init(&h->buffer);
    queryengine_init(&h->engine);
    x->handles = xrealloc(x->handles, (size_t)(x->handle_count + 1) * sizeof(Handle *));
    x->handles[x->handle_count++] = h;
    return &h->base;
}

static const PostingList *get(const Index *self, const char *term, size_t length) {
    return map_get(&((const GlobalLockIndex *)self)->map, term, length);
}

static void finish(Index *self) { (void)self; /* lists are kept sorted on every insert */ }
static int docs(const Index *self) { return atomic_load(&((GlobalLockIndex *)self)->docs); }
static int terms(const Index *self) { return (int)((const GlobalLockIndex *)self)->map.count; }
static int64_t postings(const Index *self) { return ((const GlobalLockIndex *)self)->postings; }

static void each(const Index *self, IndexVisitor visit, void *context) {
    map_each(&((const GlobalLockIndex *)self)->map, visit, context);
}

static void destroy(Index *self) {
    GlobalLockIndex *x = (GlobalLockIndex *)self;
    for (int k = 0; k < x->handle_count; k++) {
        docpostings_destroy(&x->handles[k]->doc);
        postingbuffer_destroy(&x->handles[k]->buffer);
        queryengine_destroy(&x->handles[k]->engine);
        free(x->handles[k]);
    }
    free(x->handles);
    map_destroy(&x->map);
    pthread_mutex_destroy(&x->mutex);
    free(x);
}

Index *global_lock_index_new(const Args *args) {
    GlobalLockIndex *x = xcalloc(1, sizeof(GlobalLockIndex));
    x->base.worker = worker;
    x->base.get = get;
    x->base.finish = finish;
    x->base.docs = docs;
    x->base.terms = terms;
    x->base.postings = postings;
    x->base.each = each;
    x->base.destroy = destroy;
    x->batch = args->batch;
    pthread_mutex_init(&x->mutex, NULL);
    atomic_init(&x->docs, 0);
    map_init(&x->map);
    return &x->base;
}
