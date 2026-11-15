#define _POSIX_C_SOURCE 200809L
#include "workers.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "util.h"

// What one thread needs to know.
typedef struct {
    WorkerBody body;
    void *context;
    int worker;
} Task;

// The start function of every thread.
static void *thread_main(void *arg) {
    Task *task = arg;
    task->body(task->context, task->worker);
    return NULL;
}

void workers_run(int n, WorkerBody body, void *context) {
    Task *tasks = xmalloc((size_t)n * sizeof(Task));
    pthread_t *threads = xmalloc((size_t)n * sizeof(pthread_t));
    for (int w = 0; w < n; w++) {
        tasks[w] = (Task){body, context, w};
        if (pthread_create(&threads[w], NULL, thread_main, &tasks[w]) != 0) fatal("cannot create worker thread %d", w);
    }
    for (int w = 0; w < n; w++) pthread_join(threads[w], NULL);
    free(tasks);
    free(threads);
}

// Shared by the workers of one workers_index call.
typedef struct {
    Worker **workers;
    char *const *line;
    const size_t *length;
    int from, to;
    int n;
    int static_split;
    atomic_int next;
} IndexJob;

static void index_body(void *context, int w) {
    IndexJob *job = context;
    Worker *me = job->workers[w];
    if (job->static_split) {   // worker w takes the w-th of n equal ranges of documents
        int64_t span = job->to - job->from;
        int begin = job->from + (int)(span * w / job->n), end = job->from + (int)(span * (w + 1) / job->n);
        for (int d = begin; d < end; d++) me->add(me, d, job->line[d], job->length[d]);
    } else {
        int d;
        while ((d = atomic_fetch_add(&job->next, 1)) < job->to) me->add(me, d, job->line[d], job->length[d]);
    }
    me->flush(me);   // the end of the phase: nothing may stay in a private buffer
}

void workers_index(Worker **workers, int n, char *const *line, const size_t *length, int from, int to,
                   int static_split) {
    IndexJob job = {workers, line, length, from, to, n, static_split, from};
    workers_run(n, index_body, &job);
}
