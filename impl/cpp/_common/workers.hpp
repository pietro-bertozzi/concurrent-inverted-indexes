// The worker loop shared by every variant (docs/experiment-plan.md, Rules every variant
// follows): N threads run the same body, and documents are handed out dynamically from a
// shared counter. A worker that finishes a document takes the next one, so faster cores
// simply take more documents.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "index.hpp"

// Runs body(w) for w = 0 .. n-1, each on its own thread, and waits for all of them. An exception
// thrown in a worker is rethrown here after all threads have ended.
void runWorkers(int n, const std::function<void(int)>& body);

// Indexes the documents from (inclusive) to to (exclusive): each worker repeatedly takes the
// next document ID from a shared atomic counter (d = counter++) and adds it. The counter is
// touched once per document, not once per word, so it costs almost nothing.
void indexDocs(const std::vector<Worker*>& workers, const std::vector<std::string>& docs, int from, int to);
