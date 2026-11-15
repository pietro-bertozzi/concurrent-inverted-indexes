#include "docpostings.h"

#include <stdlib.h>
#include <string.h>

#include "partitionhash.h"
#include "tokenizer.h"
#include "util.h"

#define INITIAL_TABLE 2048

void docpostings_init(DocPostings *d) {
    d->capacity = 512;
    d->count = 0;
    d->terms = xmalloc((size_t)d->capacity * sizeof(DocTerm));
    d->table_capacity = INITIAL_TABLE;
    d->table = xcalloc(d->table_capacity, sizeof(int32_t));
}

void docpostings_destroy(DocPostings *d) {
    free(d->terms);
    free(d->table);
    d->terms = NULL;
    d->table = NULL;
}

// Doubles the table and puts every term back, using the stored hashes.
static void grow_table(DocPostings *d) {
    free(d->table);
    d->table_capacity *= 2;
    d->table = xcalloc(d->table_capacity, sizeof(int32_t));
    size_t mask = d->table_capacity - 1;
    for (int k = 0; k < d->count; k++) {
        size_t i = d->terms[k].hash & mask;
        while (d->table[i] != 0) i = (i + 1) & mask;
        d->table[i] = k + 1;
        d->terms[k].slot = i;
    }
}

void docpostings_read(DocPostings *d, const char *doc, size_t length) {
    for (int k = 0; k < d->count; k++) d->table[d->terms[k].slot] = 0;   // clear only what was used
    d->count = 0;
    Tokenizer tokens;
    tokenizer_init(&tokens, doc, length);
    while (tokenizer_next(&tokens)) {
        uint64_t h = fnv1a(tokens.token, tokens.token_length);
        size_t mask = d->table_capacity - 1, i = h & mask;
        int found = 0;
        while (d->table[i] != 0) {
            DocTerm *t = &d->terms[d->table[i] - 1];
            if (t->hash == h && t->length == tokens.token_length && memcmp(t->term, tokens.token, t->length) == 0) {
                t->freq++;
                found = 1;
                break;
            }
            i = (i + 1) & mask;
        }
        if (found) continue;
        if (d->count == d->capacity) {
            d->capacity *= 2;
            d->terms = xrealloc(d->terms, (size_t)d->capacity * sizeof(DocTerm));
        }
        if ((size_t)(d->count + 1) * 2 > d->table_capacity) {   // keep the load factor at most 0.5
            grow_table(d);
            mask = d->table_capacity - 1;
            i = h & mask;
            while (d->table[i] != 0) i = (i + 1) & mask;
        }
        DocTerm *t = &d->terms[d->count];
        t->term = tokens.token;
        t->length = tokens.token_length;
        t->hash = h;
        t->freq = 1;
        t->slot = i;
        d->table[i] = ++d->count;
    }
}
