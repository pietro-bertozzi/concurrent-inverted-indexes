// The command line of docs/design.md, section CLI contract: long options only, each with one
// value; an unknown option, a missing value, a missing required option or an option that does
// not belong to the mode is an error (parseArgs throws std::runtime_error with the reason).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct Args {
    std::string mode;          // build, query, mixed-95-5, mixed-50-50, mixed-20-80
    std::string input;
    int threads = 1, repeat = 1, warmup = 0;
    std::string queries;       // query and mixed modes
    int ops = 0;               // query and mixed modes
    int preload = 0;           // mixed modes
    uint64_t seed = 0;         // query and mixed modes
    std::string latencies;     // query and mixed modes
    std::string dump;          // optional, empty if absent
    int partitions = 0, batch = 0, queue = 0;   // numeric parameters; 0 if absent (a variant checks the ones it uses)
    std::map<std::string, std::string> values;

    // True if the command line carries the numeric parameter name (partitions, batch, queue).
    bool has(const std::string& name) const { return values.count(name) > 0; }
};

// Parses and validates argv (without the program name).
Args parseArgs(const std::vector<std::string>& argv);

// The insert share in percent of mode: 100 for build, 0 for query, 5/50/80 for mixed; -1 if not a mode.
int insertShare(const std::string& mode);
