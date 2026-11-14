#include "dump.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

void writeDump(const Index& index, const std::string& file) {
    std::vector<std::pair<const std::string*, const PostingList*>> entries;
    entries.reserve(index.terms());
    index.each([&](const std::string& term, const PostingList& list) { entries.emplace_back(&term, &list); });
    std::sort(entries.begin(), entries.end(), [](const auto& x, const auto& y) { return *x.first < *y.first; });

    std::ofstream out(file, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + file);
    std::string line;
    for (const auto& [term, list] : entries) {
        line.assign(*term);
        line += '\t';
        for (size_t i = 0; i < list->size(); i++) {
            if (i > 0) line += ' ';
            line += std::to_string(list->docs[i]);
            line += ':';
            line += std::to_string(list->freqs[i]);
        }
        line += '\n';
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
    }
    out.flush();
    if (!out) throw std::runtime_error("error while writing " + file);
}
