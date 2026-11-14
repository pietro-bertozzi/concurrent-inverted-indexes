// Splits a document line into tokens. The corpus has single spaces between tokens and no
// empty tokens (docs/design.md, section Data), so a token is the text between spaces.
// A token is a view into the line: nothing is copied.
#pragma once
#include <string>
#include <string_view>

class Tokenizer {
public:
    explicit Tokenizer(const std::string& doc) : doc_(doc) {}

    // Moves to the next token; returns false when there are no more.
    bool next() {
        if (start_ >= doc_.size()) return false;
        size_t end = doc_.find(' ', start_);
        if (end == std::string_view::npos) end = doc_.size();
        token_ = doc_.substr(start_, end - start_);
        start_ = end + 1;
        return true;
    }

    // The current token (valid after next() returned true).
    std::string_view token() const { return token_; }

private:
    std::string_view doc_;
    size_t start_ = 0;
    std::string_view token_;
};
