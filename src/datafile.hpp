// the relation definition files of Bonus Project 1: parsing their text and writing relations back out
#pragma once
#include "algebra.hpp"
#include <map>
#include <sstream>

inline bool bare(const std::string& s) {
    return !s.empty() && numberEnd(s, 0) != s.size() && s.find_first_of(",()' \t\r") == std::string::npos && s.compare(0, 2, "//") != 0 && s != "}";
}
inline std::string show(const Value& v) { return v.isNum || bare(v.text) ? v.text : quoted(v.text); }
// the header then one tuple per line, in sorted order
inline void print(Rel r, std::ostream& out) {
    bool plain = std::all_of(r.cols.begin(), r.cols.end(), [&](const Col& c) { return c.rel == r.cols[0].rel; });
    for (size_t i = 0; i < r.cols.size(); i++) out << (i ? ", " : "") << (plain ? r.cols[i].name : qualified(r.cols[i]));
    out << '\n';
    sortRows(r.rows);
    for (const Row& row : r.rows) { for (size_t i = 0; i < row.size(); i++) out << (i ? ", " : "") << show(row[i]); out << '\n'; }
}

inline void skip(const std::string& s, size_t& i) { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++; }
// one value, quoted or bare
inline Value scanValue(const std::string& s, size_t& i, int ln) {
    Value v{false, 0, ""};
    if (i < s.size() && s[i] == '\'') { v.text = scanString(s, i, ln, int(i) + 1); return v; }
    size_t k = std::min(s.find(',', i), s.size());
    while (k > i && (s[k - 1] == ' ' || s[k - 1] == '\t')) k--;
    v.text = s.substr(i, k - i);
    if (v.text.empty()) throw Error{"syntax error: expected a value", ln, int(i) + 1};
    v.isNum = numberEnd(v.text, 0) == v.text.size();
    if (v.isNum) v.num = toNum(v.text, ln, int(i) + 1);
    else if (!bare(v.text)) throw Error{"lexical error: bare value must be quoted", ln, int(i) + 1};
    i = k;
    return v;
}
// relations are bags, so a tuple written twice is there twice
inline std::map<std::string, Rel> load(const std::string& text) {
    std::map<std::string, Rel> db;
    std::istringstream in(text);
    std::string line, name;
    Rel cur;
    int ln = 1;
    for (; std::getline(in, line); ln++) {
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
        size_t i = 0;
        skip(line, i);
        if (i == line.size() || line.compare(i, 2, "//") == 0) continue;
        if (name.empty()) {
            Parser p{tokenize(line, ln)};
            name = p.ident("relation name").text;
            if (db.count(name)) throw Error{"schema error: duplicate relation " + name, ln, 1};
            p.expect("(");
            do cur.cols.push_back({name, p.ident("attribute name").text, Any}); while (p.accept(","));
            p.expect(")"); p.expect("="); p.expect("{"); p.done();
            checkUnique(cur.cols, ln, 1, "definition");
        }
        else if (line[i] == '}') {
            if (i + 1 != line.size()) throw Error{"syntax error: unexpected text after '}'", ln, int(i) + 2};
            db[name] = std::move(cur); cur = {}; name.clear();
        }
        else {
            Row row;
            for (;;) {
                row.push_back(scanValue(line, i, ln));
                skip(line, i);
                if (i == line.size()) break;
                if (line[i] != ',') throw Error{"syntax error: expected ','", ln, int(i) + 1};
                skip(line, ++i);
            }
            if (row.size() != cur.cols.size()) throw Error{"syntax error: " + name + " has " + std::to_string(cur.cols.size()) + " attributes but this tuple has " + std::to_string(row.size()), ln, 1};
            for (size_t j = 0; j < row.size(); j++) {
                Type t = row[j].isNum ? RealT : StrT;
                if (cur.cols[j].type != Any && cur.cols[j].type != t) throw Error{"type error: attribute " + cur.cols[j].name + " of " + name + " mixes numbers and strings", ln, 1};
                cur.cols[j].type = t;
            }
            cur.rows.push_back(std::move(row));
        }
    }
    if (!name.empty()) throw Error{"syntax error: missing '}' for " + name, ln, 1};
    return db;
}
