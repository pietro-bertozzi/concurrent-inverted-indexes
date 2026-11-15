#include "postings.h"

#include <stdlib.h>

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
