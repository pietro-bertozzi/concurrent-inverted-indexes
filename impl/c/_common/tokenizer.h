// Splits a document line into tokens. The corpus has single spaces between tokens and no
// empty tokens (docs/design.md, section Data), so a token is the text between spaces.
// A token is a pointer into the line: nothing is copied.
#pragma once
#include <stddef.h>
#include <string.h>

typedef struct {
    const char *doc;
    size_t length;
    size_t start;
    const char *token;
    size_t token_length;
} Tokenizer;

// Starts tokenizing doc (length bytes).
static inline void tokenizer_init(Tokenizer *t, const char *doc, size_t length) {
    t->doc = doc;
    t->length = length;
    t->start = 0;
    t->token = NULL;
    t->token_length = 0;
}

// Moves to the next token (t->token, t->token_length); returns 0 when there are no more.
static inline int tokenizer_next(Tokenizer *t) {
    if (t->start >= t->length) return 0;
    const char *space = memchr(t->doc + t->start, ' ', t->length - t->start);
    size_t end = space == NULL ? t->length : (size_t)(space - t->doc);
    t->token = t->doc + t->start;
    t->token_length = end - t->start;
    t->start = end + 1;
    return 1;
}
