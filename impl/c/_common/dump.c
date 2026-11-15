#include "dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct {
    const char *term;
    size_t length;
    const PostingList *list;
} Entry;

typedef struct {
    Entry *entries;
    size_t count;
} Collector;

// each() callback: remembers one entry.
static void collect(void *context, const char *term, size_t length, const PostingList *list) {
    Collector *c = context;
    c->entries[c->count].term = term;
    c->entries[c->count].length = length;
    c->entries[c->count].list = list;
    c->count++;
}

// qsort comparison: unsigned byte order, a shorter term first if it is a prefix of the other.
static int compare(const void *x, const void *y) {
    const Entry *a = x, *b = y;
    size_t n = a->length < b->length ? a->length : b->length;
    int c = memcmp(a->term, b->term, n);
    if (c != 0) return c;
    return a->length < b->length ? -1 : (a->length > b->length ? 1 : 0);
}

void dump_write(const Index *index, const char *file) {
    Collector c = {xmalloc((size_t)index->terms(index) * sizeof(Entry)), 0};
    index->each(index, collect, &c);
    qsort(c.entries, c.count, sizeof(Entry), compare);

    FILE *out = fopen(file, "wb");
    if (out == NULL) fatal("cannot write %s", file);
    for (size_t e = 0; e < c.count; e++) {
        const PostingList *list = c.entries[e].list;
        fwrite(c.entries[e].term, 1, c.entries[e].length, out);
        fputc('\t', out);
        for (size_t i = 0; i < list->size; i++) {
            if (i > 0) fputc(' ', out);
            fprintf(out, "%d:%d", list->docs[i], list->freqs[i]);
        }
        fputc('\n', out);
    }
    free(c.entries);
    if (fclose(out) != 0) fatal("error while writing %s", file);
}
