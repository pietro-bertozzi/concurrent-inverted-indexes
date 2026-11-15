#define _POSIX_C_SOURCE 200809L
#include "global_lock_index.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "docpostings.h"
#include "hashmap.h"
#include "queryengine.h"
#include "util.h"

typedef struct GlobalLockIndex GlobalLockIndex;

// One worker's private state: its tokenizing buffer and its query scratch space.
typedef struct {
    Worker base;       // must be first
    GlobalLockIndex *index;
    DocPostings doc;
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

static void add(Worker *self, int doc_id, const char *text, size_t length) {
    Handle *h = (Handle *)self;
    GlobalLockIndex *x = h->index;
    docpostings_read(&h->doc, text, length);   // no lock: private work
    int n = h->doc.count;
    for (int from = 0; from < n; from += x->batch) {
        int to = n - from > x->batch ? from + x->batch : n;
        pthread_mutex_lock(&x->mutex);
        for (int k = from; k < to; k++) {
            const DocTerm *t = &h->doc.terms[k];
            int created;
            PostingList *list = map_get_or_create(&x->map, t->term, t->length, &created);
            postings_insert(list, doc_id, t->freq);
        }
        x->postings += to - from;
        pthread_mutex_unlock(&x->mutex);
        if (to == n) break;
    }
    atomic_fetch_add(&x->docs, 1);
}

static int query(Worker *self, const Queries *q, int i) {
    Handle *h = (Handle *)self;
    pthread_mutex_lock(&h->index->mutex);
    int size = queryengine_run(&h->engine, &h->index->base, q, i);
    pthread_mutex_unlock(&h->index->mutex);
    return size;
}

static Worker *worker(Index *self) {
    GlobalLockIndex *x = (GlobalLockIndex *)self;
    Handle *h = xcalloc(1, sizeof(Handle));
    h->base.add = add;
    h->base.query = query;
    h->index = x;
    docpostings_init(&h->doc);
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
