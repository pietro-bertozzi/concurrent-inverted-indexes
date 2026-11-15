// The benchmark harness shared by every C++ variant: parses the command line, runs the
// repetitions (warm-ups first) and prints one TSV row per repetition on stdout, in the
// measurement columns of docs/design.md, section Results. Logs go to stderr.
//
// Every repetition starts from scratch: the input is read again, the index is new, the
// generators restart from --seed, so all repetitions do identical work.
//
// Every phase runs on N worker threads (also with N = 1): documents are handed out from a
// shared counter (indexDocs), and each worker follows its own operation plan (OpPlan).
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "index.hpp"

#include "args.hpp"

// Makes an empty index from the command line (the variant reads the numeric parameters it uses).
using IndexFactory = std::function<std::unique_ptr<Index>(const Args&)>;

// Entry point of a variant: maxThreads is the highest worker count it supports, factory makes
// an empty index, uses lists the numeric parameters (partitions, batch, queue) the variant
// accepts. Returns the exit status: 0, or 1 after an error message on stderr.
int runVariant(int argc, char** argv, int maxThreads, const IndexFactory& factory,
               const std::vector<std::string>& uses);
