#include "args.hpp"

#include <cerrno>
#include <cstdlib>
#include <set>
#include <stdexcept>

namespace {

const std::set<std::string> KNOWN = {"mode", "input", "threads", "repeat", "warmup", "queries", "ops",
                                      "preload", "seed", "partitions", "batch", "queue", "latencies", "dump"};

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

std::string required(const Args& a, const std::string& name) {
    auto it = a.values.find(name);
    if (it == a.values.end()) {
        auto m = a.values.find("mode");
        fail("missing required option --" + name + " for mode " + (m == a.values.end() ? "" : m->second));
    }
    return it->second;
}

void forbidden(const Args& a, const std::string& name) {
    if (a.has(name)) fail("option --" + name + " does not apply to mode " + a.values.at("mode"));
}

// Reads the required option name as an integer >= min that fits in an int32.
int integer(const Args& a, const std::string& name, int min) {
    std::string v = required(a, name);
    if (allDigits(v) && v.size() <= 10) {
        long long n = std::stoll(v);
        if (n <= 2147483647LL && n >= min) return static_cast<int>(n);
    }
    fail("option --" + name + " must be an integer >= " + std::to_string(min) + ", got '" + v + "'");
}

}  // namespace

int insertShare(const std::string& mode) {
    if (mode == "build") return 100;
    if (mode == "query") return 0;
    // mixed-<query share>-<insert share>
    if (mode.rfind("mixed-", 0) != 0) return -1;
    size_t dash = mode.rfind('-');
    std::string first = mode.substr(6, dash > 6 ? dash - 6 : 0), second = mode.substr(dash + 1);
    if (dash <= 6 || !allDigits(first) || !allDigits(second) || second.size() > 9) return -1;
    int share = std::stoi(second);
    return share <= 100 ? share : -1;
}

Args parseArgs(const std::vector<std::string>& argv) {
    Args a;
    for (size_t i = 0; i < argv.size(); i += 2) {
        if (argv[i].rfind("--", 0) != 0) fail("expected an option, got '" + argv[i] + "'");
        std::string name = argv[i].substr(2);
        if (!KNOWN.count(name)) fail("unknown option " + argv[i]);
        if (i + 1 >= argv.size()) fail("option " + argv[i] + " has no value");
        if (!a.values.emplace(name, argv[i + 1]).second) fail("option " + argv[i] + " given twice");
    }
    a.mode = required(a, "mode");
    bool query = a.mode == "query", mixed = a.mode.rfind("mixed-", 0) == 0;
    if (insertShare(a.mode) < 0) fail("unknown mode '" + a.mode + "'");
    a.input = required(a, "input");
    a.threads = integer(a, "threads", 1);
    a.repeat = integer(a, "repeat", 1);
    a.warmup = integer(a, "warmup", 0);
    // Options that only some modes use: required there, an error elsewhere.
    for (const char* name : {"queries", "ops", "seed", "latencies"}) {
        if (query || mixed) required(a, name); else forbidden(a, name);
    }
    if (mixed) required(a, "preload"); else forbidden(a, "preload");
    if (query || mixed) {
        a.queries = a.values.at("queries");
        a.latencies = a.values.at("latencies");
        a.ops = integer(a, "ops", 0);
        const std::string& s = a.values.at("seed");
        errno = 0;
        char* end = nullptr;
        a.seed = std::strtoull(s.c_str(), &end, 10);
        if (!allDigits(s) || errno != 0 || *end != '\0') fail("option --seed must be an unsigned integer, got '" + s + "'");
    }
    if (mixed) a.preload = integer(a, "preload", 0);
    if (a.has("dump")) a.dump = a.values.at("dump");
    return a;
}
