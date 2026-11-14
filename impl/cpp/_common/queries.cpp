#include "queries.hpp"

#include <fstream>
#include <stdexcept>

Queries loadQueries(const std::string& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("cannot open " + file);
    Queries q;
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        number++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;                         // split at single spaces
        size_t start = 0;
        for (;;) {
            size_t end = line.find(' ', start);
            f.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        int kind;
        if (f[0] == "SIMPLE") kind = Queries::SIMPLE;
        else if (f[0] == "AND") kind = Queries::AND;
        else if (f[0] == "OR") kind = Queries::OR;
        else throw std::runtime_error(file + " line " + std::to_string(number) + ": unknown operator '" + f[0] + "'");
        if (f.size() != (kind == Queries::SIMPLE ? 2u : 3u)) {
            throw std::runtime_error(file + " line " + std::to_string(number) + ": wrong number of terms for " + f[0]);
        }
        q.kind.push_back(kind);
        q.first.push_back(f[1]);
        q.second.push_back(kind == Queries::SIMPLE ? "" : f[2]);
    }
    if (q.size() == 0) throw std::runtime_error(file + " has no queries");
    return q;
}
