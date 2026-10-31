// JSON values, a parser that reports where it failed, and a writer
#pragma once
#include "lexer.hpp"
#include <algorithm>
#include <cstdio>
#include <utility>

struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    // a string's contents, a number's literal text, or true/false
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;
    int line = 0, col = 0;

    const Json* find(const std::string& key) const {
        for (const auto& [k, v] : fields) if (k == key) return &v;
        return nullptr;
    }
    Json& add(const std::string& key, Json value) { fields.emplace_back(key, std::move(value)); return *this; }
    Json& push(Json value) { items.push_back(std::move(value)); return *this; }
    bool scalar() const { return kind != Array && kind != Object; }
};
inline Json jsonString(const std::string& s) { Json j; j.kind = Json::String; j.text = s; return j; }
inline Json jsonNumber(const std::string& literal) { Json j; j.kind = Json::Number; j.text = literal; return j; }
inline Json jsonNumber(long long n) { return jsonNumber(std::to_string(n)); }
inline Json jsonBool(bool b) { Json j; j.kind = Json::Bool; j.text = b ? "true" : "false"; return j; }
inline Json jsonArray() { Json j; j.kind = Json::Array; return j; }
inline Json jsonObject() { Json j; j.kind = Json::Object; return j; }

// the parser recurses once per array or object, so a document nested deeper than any plan is an error
// rather than a stack overflow
constexpr int MAX_JSON_DEPTH = 1000;
struct JsonParser {
    const std::string& s; size_t i = 0; int line = 1, depth = 0; size_t lineStart = 0;
    int col() const { return int(i - lineStart) + 1; }
    [[noreturn]] void fail(const std::string& what) {
        std::string found = i >= s.size() ? "end of input" : "'" + std::string(1, s[i]) + "'";
        throw Error{"json syntax error: expected " + what + " but found " + found, line, col()};
    }
    void space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
            if (s[i++] == '\n') { line++; lineStart = i; }
    }
    bool accept(char c) { space(); if (i < s.size() && s[i] == c) { i++; return true; } return false; }
    void expect(char c) { if (!accept(c)) fail(std::string("'") + c + "'"); }

    Json document() { Json j = value(); space(); if (i != s.size()) fail("end of input"); return j; }
    Json value() {
        space();
        Json j;
        j.line = line; j.col = col();
        if (i >= s.size()) fail("a value");
        char c = s[i];
        if ((c == '{' || c == '[') && ++depth > MAX_JSON_DEPTH)
            throw Error{"json syntax error: arrays and objects nested more than " + std::to_string(MAX_JSON_DEPTH) + " deep", line, col()};
        if (c == '{') {
            j.kind = Json::Object; i++;
            if (accept('}')) { depth--; return j; }
            do {
                space();
                if (i >= s.size() || s[i] != '"') fail("a string key");
                std::string key = string();
                expect(':');
                j.fields.emplace_back(key, value());
            } while (accept(','));
            if (!accept('}')) fail("',' or '}'");
            depth--;
        }
        else if (c == '[') {
            j.kind = Json::Array; i++;
            if (accept(']')) { depth--; return j; }
            do j.items.push_back(value()); while (accept(','));
            if (!accept(']')) fail("',' or ']'");
            depth--;
        }
        else if (c == '"') { j.kind = Json::String; j.text = string(); }
        else if (c == '-' || dig(c)) { j.kind = Json::Number; j.text = number(); }
        else if (word("true")) { j.kind = Json::Bool; j.text = "true"; }
        else if (word("false")) { j.kind = Json::Bool; j.text = "false"; }
        else if (word("null")) j.kind = Json::Null;
        else fail("a value");
        return j;
    }
    bool word(const char* w) {
        size_t n = std::char_traits<char>::length(w);
        if (s.compare(i, n, w) != 0) return false;
        i += n;
        return true;
    }
    std::string number() {
        size_t start = i;
        if (s[i] == '-') i++;
        if (i >= s.size() || !dig(s[i])) fail("a digit");
        if (s[i] == '0') i++; else while (i < s.size() && dig(s[i])) i++;
        if (i < s.size() && s[i] == '.') { i++; if (i >= s.size() || !dig(s[i])) fail("a digit"); while (i < s.size() && dig(s[i])) i++; }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            i++;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
            if (i >= s.size() || !dig(s[i])) fail("a digit");
            while (i < s.size() && dig(s[i])) i++;
        }
        return s.substr(start, i - start);
    }
    unsigned hex4() {
        unsigned code = 0;
        for (int k = 0; k < 4; k++, i++) {
            if (i >= s.size()) fail("a hex digit");
            char c = s[i];
            unsigned d = dig(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (d == 16) fail("a hex digit");
            code = code * 16 + d;
        }
        return code;
    }
    static void utf8(std::string& out, unsigned code) {
        if (code < 0x80) out += char(code);
        else if (code < 0x800) { out += char(0xC0 | code >> 6); out += char(0x80 | (code & 0x3F)); }
        else if (code < 0x10000) { out += char(0xE0 | code >> 12); out += char(0x80 | (code >> 6 & 0x3F)); out += char(0x80 | (code & 0x3F)); }
        else { out += char(0xF0 | code >> 18); out += char(0x80 | (code >> 12 & 0x3F)); out += char(0x80 | (code >> 6 & 0x3F)); out += char(0x80 | (code & 0x3F)); }
    }
    std::string string() {
        std::string out;
        for (i++;; i++) {
            if (i >= s.size() || s[i] == '\n') fail("'\"' to close the string");
            char c = s[i];
            if (c == '"') { i++; return out; }
            if ((unsigned char)c < 0x20) fail("no control character in a string");
            if (c != '\\') { out += c; continue; }
            if (++i >= s.size()) fail("an escape");
            switch (s[i]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    i++;
                    unsigned code = hex4();
                    if (code >= 0xD800 && code < 0xDC00 && s.compare(i, 2, "\\u") == 0) {
                        i += 2;
                        unsigned low = hex4();
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    utf8(out, code);
                    i--;
                    break;
                }
                default: fail("an escape");
            }
        }
    }
};
inline Json parseJson(const std::string& text) { return JsonParser{text}.document(); }

inline std::string jsonQuote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if ((unsigned char)c < 0x20) { char code[8]; std::snprintf(code, sizeof code, "\\u%04x", c); out += code; }
        else out += c;
    }
    return out + "\"";
}
// arrays of scalars stay on one line so rows and column lists read like the handout's examples
inline void writeJson(const Json& j, std::string& out, int indent, int depth = 0) {
    std::string pad = indent < 0 ? "" : "\n" + std::string((depth + 1) * indent, ' '), end = indent < 0 ? "" : "\n" + std::string(depth * indent, ' ');
    switch (j.kind) {
        case Json::Null: out += "null"; return;
        case Json::Bool: case Json::Number: out += j.text; return;
        case Json::String: out += jsonQuote(j.text); return;
        case Json::Array: {
            bool flat = std::all_of(j.items.begin(), j.items.end(), [](const Json& x) { return x.scalar(); });
            out += '[';
            for (size_t k = 0; k < j.items.size(); k++) { out += k ? "," : ""; out += flat ? (k && indent >= 0 ? " " : "") : pad; writeJson(j.items[k], out, indent, depth + 1); }
            out += flat || j.items.empty() ? "]" : end + "]";
            return;
        }
        case Json::Object: {
            out += '{';
            for (size_t k = 0; k < j.fields.size(); k++) { out += k ? "," : ""; out += pad + jsonQuote(j.fields[k].first) + (indent < 0 ? ":" : ": "); writeJson(j.fields[k].second, out, indent, depth + 1); }
            out += j.fields.empty() ? "}" : end + "}";
            return;
        }
    }
}
inline std::string dumpJson(const Json& j, int indent = 2) { std::string out; writeJson(j, out, indent); return out; }
