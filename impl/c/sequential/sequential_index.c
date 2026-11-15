#include "sequential_index.h"

#include <stdlib.h>

#include "hashmap.h"
#include "tokenizer.h"
#include "util.h"

typedef struct {
    Index base;        // must be first: the harness sees only this part
    TermMap map;
    int docs;
    int64_t postings;
} SequentialIndex;

static void add(Index *self, int doc_id, const char *doc, size_t length) {
    SequentialIndex *s = (SequentialIndex *)self;
    Tokenizer tokens;
    tokenizer_init(&tokens, doc, length);
    while (tokenizer_next(&tokens)) {
        // The map looks the token up by (pointer, length): no copy; the key is copied only
        // when the term is new.
        int created;
        PostingList *list = map_get_or_create(&s->map, tokens.token, tokens.token_length, &created);
        // A term seen earlier in this document is the last posting of its list: count it.
        if (list->size > 0 && list->docs[list->size - 1] == doc_id) {
            list->freqs[list->size - 1]++;
        } else {
            postings_append(list, doc_id, 1);
            s->postings++;
        }
    }
    s->docs++;
}

static const PostingList *get(const Index *self, const char *term, size_t length) {
    return map_get(&((const SequentialIndex *)self)->map, term, length);
}

static void finish(Index *self) { (void)self; /* sorted by construction */ }
static int docs(const Index *self) { return ((const SequentialIndex *)self)->docs; }
static int terms(const Index *self) { return (int)((const SequentialIndex *)self)->map.count; }
static int64_t postings(const Index *self) { return ((const SequentialIndex *)self)->postings; }

static void each(const Index *self, IndexVisitor visit, void *context) {
    map_each(&((const SequentialIndex *)self)->map, visit, context);
}

static void destroy(Index *self) {
    SequentialIndex *s = (SequentialIndex *)self;
    map_destroy(&s->map);
    free(s);
}

Index *sequential_index_new(void) {
    SequentialIndex *s = xcalloc(1, sizeof(SequentialIndex));
    s->base.add = add;
    s->base.get = get;
    s->base.finish = finish;
    s->base.docs = docs;
    s->base.terms = terms;
    s->base.postings = postings;
    s->base.each = each;
    s->base.destroy = destroy;
    map_init(&s->map);
    return &s->base;
}
