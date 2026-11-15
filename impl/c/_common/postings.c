#include "postings.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

PostingList *postings_new(void) { return xcalloc(1, sizeof(PostingList)); }

void postings_free(PostingList *list) {
    if (list == NULL) return;
    free(list->docs);
    free(list->freqs);
    free(list);
}

void postings_append(PostingList *list, int doc_id, int freq) {
    if (list->size == list->capacity) {
        list->capacity = list->capacity == 0 ? 2 : list->capacity * 2;
        list->docs = xrealloc(list->docs, list->capacity * sizeof(int32_t));
        list->freqs = xrealloc(list->freqs, list->capacity * sizeof(int32_t));
    }
    list->docs[list->size] = (int32_t)doc_id;
    list->freqs[list->size] = (int32_t)freq;
    list->size++;
}

void postings_insert(PostingList *list, int doc_id, int freq) {
    size_t pos = list->size;
    while (pos > 0 && list->docs[pos - 1] > doc_id) pos--;   // walk back from the end
    postings_append(list, doc_id, freq);                      // grows the arrays if needed
    if (pos < list->size - 1) {                               // not the end: shift the tail one slot up
        size_t tail = list->size - 1 - pos;
        memmove(list->docs + pos + 1, list->docs + pos, tail * sizeof(int32_t));
        memmove(list->freqs + pos + 1, list->freqs + pos, tail * sizeof(int32_t));
        list->docs[pos] = (int32_t)doc_id;
        list->freqs[pos] = (int32_t)freq;
    }
}
