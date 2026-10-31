// tokens and the character by character scanner
#pragma once
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

// the position of the first byte that does not begin well-formed UTF-8 (RFC 3629: no overlong forms, no
// surrogates, nothing past U+10FFFF), or npos
inline size_t invalidUtf8At(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        unsigned char lead = (unsigned char)text[i];
        if (lead < 0x80) { i++; continue; }
        size_t length = lead >= 0xC2 && lead <= 0xDF ? 2 : lead >= 0xE0 && lead <= 0xEF ? 3 : lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
        if (length == 0 || i + length > text.size()) return i;
        unsigned code = lead & (0x7F >> length);
        for (size_t k = 1; k < length; k++) {
            unsigned char next = (unsigned char)text[i + k];
            if ((next & 0xC0) != 0x80) return i;
            code = code << 6 | (next & 0x3F);
        }
        unsigned smallest = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
        if (code < smallest || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) return i;
        i += length;
    }
    return std::string::npos;
}

struct Error { std::string msg; int line = 0, col = 0; };

enum Tok { Word, Num, Str, Sym, End };
const char* const TOK[] = {"word", "number", "string", "symbol", "end"};

struct Token { Tok kind; std::string text; int line, col; };

inline bool dig(char c) { return c >= '0' && c <= '9'; }
inline bool wordChar(char c) { return dig(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

// end of a number starting at i, or i if there isn't one
inline size_t numberEnd(const std::string& s, size_t i) {
    size_t j = i;
    if (j < s.size() && s[j] == '-') j++;
    if (j == s.size() || !dig(s[j])) return i;
    while (j < s.size() && dig(s[j])) j++;
    if (j + 1 < s.size() && s[j] == '.' && dig(s[j + 1]))
        while (++j < s.size() && dig(s[j])) {}
    return j;
}

inline double toNum(const std::string& text, int line, int col) {
    double d = std::strtod(text.c_str(), nullptr);
    if (!std::isfinite(d)) throw Error{"lexical error: number out of range", line, col};
    return d;
}

// reads a quoted string, '' is one quote
inline std::string scanString(const std::string& s, size_t& i, int line, int col) {
    std::string t;
    for (i++;; i++) {
        if (i >= s.size() || s[i] == '\n') throw Error{"lexical error: unterminated string", line, col};
        if (s[i] != '\'') t += s[i];
        else if (i + 1 < s.size() && s[i + 1] == '\'') { t += '\''; i++; }
        else { i++; return t; }
    }
}
inline std::string quoted(const std::string& t) {
    std::string s = "'";
    for (char c : t) s += c == '\'' ? "''" : std::string(1, c);
    return s + "'";
}

inline std::vector<Token> tokenize(const std::string& s, int line = 1) {
    std::vector<Token> out;
    size_t start = 0;
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        int col = int(i - start) + 1;
        auto emit = [&](Tok k, const std::string& t) { out.push_back({k, t, line, col}); };
        if (c == '\n') { line++; start = ++i; }
        else if (c == ' ' || c == '\t' || c == '\r') i++;
        else if (s.compare(i, 2, "//") == 0) while (i < s.size() && s[i] != '\n') i++;
        else if (wordChar(c) && !dig(c)) {
            size_t j = i;
            while (j < s.size() && wordChar(s[j])) j++;
            emit(Word, s.substr(i, j - i)); i = j;
        }
        else if (size_t j = numberEnd(s, i); j > i) { emit(Num, s.substr(i, j - i)); i = j; }
        else if (c == '\'') emit(Str, scanString(s, i, line, col));
        else if (c == '<' || c == '>' || c == '!') {   // maximal munch
            size_t len = i + 1 < s.size() && s[i + 1] == '=' ? 2 : 1;
            if (c == '!' && len == 1) throw Error{"lexical error: unexpected character '!'", line, col};
            emit(Sym, s.substr(i, len)); i += len;
        }
        else if (std::string("()[]{},.=").find(c) != std::string::npos) { emit(Sym, std::string(1, c)); i++; }
        else {
            size_t j = i + 1;
            while (j < s.size() && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
            throw Error{"lexical error: unexpected character '" + s.substr(i, j - i) + "'", line, col};
        }
    }
    out.push_back({End, "end of input", line, int(s.size() - start) + 1});
    return out;
}
