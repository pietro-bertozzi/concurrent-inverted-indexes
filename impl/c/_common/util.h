// Small helpers shared by all C sources: fatal errors and memory allocation that cannot fail
// silently. Errors go to stderr and end the process with exit status 1 (docs/design.md,
// section CLI contract: stdout = TSV only, stderr = logs, exit code = success/failure).
#pragma once
#include <stddef.h>

// Prints "error: <message>" (printf-style) to stderr and exits with status 1.
void fatal(const char *format, ...) __attribute__((noreturn, format(printf, 1, 2)));

// malloc / calloc / realloc that call fatal() when memory runs out.
void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *p, size_t size);
