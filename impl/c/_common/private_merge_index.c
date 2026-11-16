#include "private_merge_index.h"

#include <stdlib.h>

#include "docpostings.h"
#include "hashmap.h"
#include "partitionhash.h"
#include "queryengine.h"
#include "util.h"
#include "workers.h"

typedef struct PrivateMergeIndex PrivateMergeIndex;

// One worker's private index plus its private scratch state.
typedef struct {
    Worker base;          // must be first
    PrivateMergeIndex *index;
    DocPostings doc;
    QueryEngine engine;
    TermMap *piles;       // piles[i]: the terms of pile i (n entries)
    int docs;
    int64_t postings;
} Handle;

struct PrivateMergeIndex {
    Index base;           // must be first: the harness sees only this part
    int n;                // workers = piles
    int heap;             // 1 = merge with a binary min-heap, 0 = linear scan
    Handle **handles;     // the handles made so far, freed in destroy
    int handle_count;
    TermMap *merged;      // merged[i]: the terms of pile i, set by finish()
    int docs;
    int64_t postings;
};

static void add(Worker *self, int doc_id, const char *text, size_t length) {
    Handle *h = (Handle *)self;
    docpostings_read(&h->doc, text, length);
    for (int k = 0; k < h->doc.count; k++) {
        const DocTerm *t = &h->doc.terms[k];
        int created;
        PostingList *list = map_get_or_create(&h->piles[partition_of_hash(t->hash, h->index->n)], t->term, t->length, &created);
        postings_append(list, doc_id, t->freq);   // IDs of one worker only grow: the list stays sorted
    }
    h->docs++;
    h->postings += h->doc.count;
}

static int query(Worker *self, const Queries *q, int i) {
    Handle *h = (Handle *)self;
    return queryengine_run(&h->engine, &h->index->base, q, i);
}

static void flush(Worker *self) { (void)self; /* nothing is buffered: the private index is the buffer */ }

static Worker *worker(Index *self, int number) {
    (void)number;
    PrivateMergeIndex *x = (PrivateMergeIndex *)self;
    Handle *h = xcalloc(1, sizeof(Handle));
    h->base.add = add;
    h->base.query = query;
    h->base.flush = flush;
    h->index = x;
    docpostings_init(&h->doc);
    queryengine_init(&h->engine);
    h->piles = xcalloc((size_t)x->n, sizeof(TermMap));
    for (int i = 0; i < x->n; i++) map_init(&h->piles[i]);
    x->handles = xrealloc(x->handles, (size_t)(x->handle_count + 1) * sizeof(Handle *));
    x->handles[x->handle_count++] = h;
    return &h->base;
}

// The lists of one term that more than one worker has: the first one (it lives in the merged
// map) and the others, merged together at the end of the pile.
typedef struct {
    PostingList *first;
    PostingList **others;
    int count, capacity;
} Multi;

// Finds Multi records by the address of the first list: open addressing, linear probing.
typedef struct {
    Multi *items;
    int count, capacity;
    int32_t *table;       // table[slot] = index in items + 1, 0 = empty
    size_t table_capacity;   // power of two
} MultiSet;

static size_t pointer_slot(const PostingList *p, size_t mask) { return (((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ULL >> 20) & mask; }

// Returns the Multi of `first`, adding an empty one if there is none.
static Multi *multiset_get(MultiSet *s, PostingList *first) {
    if (2 * (size_t)(s->count + 1) > s->table_capacity) {   // keep the load factor under 0.5
        size_t capacity = s->table_capacity == 0 ? 1024 : s->table_capacity * 2;
        free(s->table);
        s->table = xcalloc(capacity, sizeof(int32_t));
        s->table_capacity = capacity;
        for (int k = 0; k < s->count; k++) {
            size_t j = pointer_slot(s->items[k].first, capacity - 1);
            while (s->table[j] != 0) j = (j + 1) & (capacity - 1);
            s->table[j] = k + 1;
        }
    }
    size_t mask = s->table_capacity - 1, j = pointer_slot(first, mask);
    while (s->table[j] != 0) {
        if (s->items[s->table[j] - 1].first == first) return &s->items[s->table[j] - 1];
        j = (j + 1) & mask;
    }
    if (s->count == s->capacity) {
        s->capacity = s->capacity == 0 ? 256 : s->capacity * 2;
        s->items = xrealloc(s->items, (size_t)s->capacity * sizeof(Multi));
    }
    Multi *m = &s->items[s->count];
    m->first = first;
    m->others = NULL;
    m->count = m->capacity = 0;
    s->table[j] = ++s->count;
    return m;
}

// Linear-scan merge of k sorted lists with disjoint document IDs into `out` (arrays of `total`
// entries): for every posting, scan the k heads and take the smallest. O(k) per posting.
static void merge_linear(PostingList **lists, int k, size_t total, PostingList *out) {
    size_t *at = xcalloc((size_t)k, sizeof(size_t));
    for (size_t n = 0; n < total; n++) {
        int best = -1;
        for (int j = 0; j < k; j++)
            if (at[j] < lists[j]->size && (best < 0 || lists[j]->docs[at[j]] < lists[best]->docs[at[best]])) best = j;
        out->docs[n] = lists[best]->docs[at[best]];
        out->freqs[n] = lists[best]->freqs[at[best]];
        at[best]++;
    }
    free(at);
}

// Moves the entry at position i of the heap (key = head document ID, src = the list it comes
// from) down until both its children have larger keys.
static void sift_down(int32_t *key, int *src, int size, int i) {
    int32_t k0 = key[i];
    int s0 = src[i];
    for (;;) {
        int c = 2 * i + 1;                                   // left child
        if (c >= size) break;
        if (c + 1 < size && key[c + 1] < key[c]) c++;        // the smaller child
        if (key[c] >= k0) break;
        key[i] = key[c];
        src[i] = src[c];
        i = c;
    }
    key[i] = k0;
    src[i] = s0;
}

// The same merge with a binary min-heap: it holds one entry per list that is not exhausted, the
// head document ID (key) and the list it comes from (src), with the smallest key at the root.
// Each posting takes the root, then replaces it by the next head of the same list or, if the
// list is exhausted, by the last entry, and sifts it down. O(log k) per posting.
static void merge_heap(PostingList **lists, int k, size_t total, PostingList *out) {
    size_t *at = xcalloc((size_t)k, sizeof(size_t));
    int32_t *key = xmalloc((size_t)k * sizeof(int32_t));
    int *src = xmalloc((size_t)k * sizeof(int));
    int size = 0;
    for (int j = 0; j < k; j++) {
        if (lists[j]->size > 0) {
            key[size] = lists[j]->docs[0];
            src[size] = j;
            size++;
        }
    }
    for (int i = size / 2 - 1; i >= 0; i--) sift_down(key, src, size, i);   // build the heap
    for (size_t n = 0; n < total; n++) {
        int j = src[0];
        out->docs[n] = key[0];
        out->freqs[n] = lists[j]->freqs[at[j]];
        at[j]++;
        if (at[j] < lists[j]->size) {
            key[0] = lists[j]->docs[at[j]];   // the next head of the same list
        } else {
            size--;                           // list exhausted: the last entry takes the root
            key[0] = key[size];
            src[0] = src[size];
        }
        sift_down(key, src, size, 0);
    }
    free(src);
    free(key);
    free(at);
}

// N-way merge of the first list and the others (sorted, disjoint document IDs), with the
// linear scan or, if `heap` is set, the binary min-heap. The result replaces the first list;
// the arrays of the others are freed.
static void merge_multi(Multi *m, int heap) {
    int k = m->count + 1;
    PostingList **lists = xmalloc((size_t)k * sizeof(PostingList *));
    PostingList old = *m->first;                 // the first list's arrays, still to be read
    PostingList *first = m->first;
    lists[0] = &old;
    for (int j = 0; j < m->count; j++) lists[j + 1] = m->others[j];
    size_t total = 0;
    for (int j = 0; j < k; j++) total += lists[j]->size;
    first->docs = xmalloc(total * sizeof(int32_t));
    first->freqs = xmalloc(total * sizeof(int32_t));
    first->size = first->capacity = total;
    if (heap) merge_heap(lists, k, total, first);
    else merge_linear(lists, k, total, first);
    for (int j = 0; j < k; j++) {
        free(lists[j]->docs);
        free(lists[j]->freqs);
        lists[j]->docs = lists[j]->freqs = NULL;
        lists[j]->size = lists[j]->capacity = 0;
    }
    free(lists);
}

// Builds merged pile i: for every term, the N-way merge of its lists in pile i of each worker.
// The first list of a term moves into the merged map (its arrays are taken over, not copied);
// later lists of the same term are collected in a Multi and merged at the end.
static void merge_pile(void *context, int i) {
    PrivateMergeIndex *x = context;
    TermMap *result = &x->merged[i];
    MultiSet multi = {0};
    for (int w = 0; w < x->handle_count; w++) {
        TermMap *pile = &x->handles[w]->piles[i];
        for (size_t s = 0; s < pile->capacity; s++) {
            MapSlot *slot = &pile->slots[s];
            if (slot->key == NULL) continue;
            int created;
            PostingList *dst = map_get_or_create(result, slot->key, slot->key_length, &created);
            if (created) {
                *dst = *slot->list;   // take over the arrays
            } else {
                Multi *m = multiset_get(&multi, dst);
                if (m->count == m->capacity) {
                    m->capacity = m->capacity == 0 ? 2 : m->capacity * 2;
                    m->others = xrealloc(m->others, (size_t)m->capacity * sizeof(PostingList *));
                }
                m->others[m->count++] = slot->list;
            }
            if (created) {
                slot->list->docs = slot->list->freqs = NULL;
                slot->list->size = slot->list->capacity = 0;
            }
        }
    }
    for (int k = 0; k < multi.count; k++) {
        merge_multi(&multi.items[k], x->heap);
        free(multi.items[k].others);
    }
    free(multi.items);
    free(multi.table);
}

// Merges the piles: thread i builds the merged pile i from pile i of every worker.
static void finish(Index *self) {
    PrivateMergeIndex *x = (PrivateMergeIndex *)self;
    x->merged = xcalloc((size_t)x->n, sizeof(TermMap));
    for (int i = 0; i < x->n; i++) map_init(&x->merged[i]);
    workers_run(x->n, merge_pile, x);
    for (int w = 0; w < x->handle_count; w++) {
        Handle *h = x->handles[w];
        x->docs += h->docs;
        x->postings += h->postings;
        for (int i = 0; i < x->n; i++) map_destroy(&h->piles[i]);   // the private indexes are no longer needed
        free(h->piles);
        h->piles = NULL;
    }
}

static const PostingList *get(const Index *self, const char *term, size_t length) {
    const PrivateMergeIndex *x = (const PrivateMergeIndex *)self;
    return map_get(&x->merged[partition_of(term, length, x->n)], term, length);
}

static int docs(const Index *self) { return ((const PrivateMergeIndex *)self)->docs; }

static int terms(const Index *self) {
    const PrivateMergeIndex *x = (const PrivateMergeIndex *)self;
    size_t n = 0;
    if (x->merged != NULL) for (int i = 0; i < x->n; i++) n += x->merged[i].count;
    return (int)n;
}

static int64_t postings(const Index *self) { return ((const PrivateMergeIndex *)self)->postings; }

static void each(const Index *self, IndexVisitor visit, void *context) {
    const PrivateMergeIndex *x = (const PrivateMergeIndex *)self;
    for (int i = 0; i < x->n; i++) map_each(&x->merged[i], visit, context);
}

static void destroy(Index *self) {
    PrivateMergeIndex *x = (PrivateMergeIndex *)self;
    for (int k = 0; k < x->handle_count; k++) {
        Handle *h = x->handles[k];
        docpostings_destroy(&h->doc);
        queryengine_destroy(&h->engine);
        if (h->piles != NULL) {
            for (int i = 0; i < x->n; i++) map_destroy(&h->piles[i]);
            free(h->piles);
        }
        free(h);
    }
    free(x->handles);
    if (x->merged != NULL) {
        for (int i = 0; i < x->n; i++) map_destroy(&x->merged[i]);
        free(x->merged);
    }
    free(x);
}

// Makes an empty index for args->threads workers; static_split chooses how documents are handed out,
// heap chooses the merge of a term's lists (binary min-heap or linear scan).
static Index *new_index(const Args *args, int static_split, int heap) {
    PrivateMergeIndex *x = xcalloc(1, sizeof(PrivateMergeIndex));
    x->base.worker = worker;
    x->base.get = get;
    x->base.finish = finish;
    x->base.docs = docs;
    x->base.terms = terms;
    x->base.postings = postings;
    x->base.each = each;
    x->base.destroy = destroy;
    x->base.static_split = static_split;
    x->n = args->threads;
    x->heap = heap;
    return &x->base;
}

Index *private_merge_index_new(const Args *args) { return new_index(args, 0, 0); }

Index *private_merge_static_index_new(const Args *args) { return new_index(args, 1, 0); }

Index *private_merge_heap_index_new(const Args *args) { return new_index(args, 0, 1); }
