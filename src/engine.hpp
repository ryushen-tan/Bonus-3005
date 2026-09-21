// relations, binding, the operators, output and loading
#pragma once
#include "parser.hpp"
#include <algorithm>
#include <map>
#include <sstream>

enum Type { Any, NumT, StrT };
const char* const TYPE[] = {"any", "number", "string"};
struct Col { std::string rel, name; Type type; };
using Row = std::vector<Value>;
struct Rel { std::vector<Col> cols; std::vector<Row> rows; };

// our own ordering and equality for values and tuples
inline int compare(const Value& a, const Value& b) { return a.isNum ? (a.num < b.num ? -1 : a.num > b.num) : a.text.compare(b.text); }
inline int compareRow(const Row& a, const Row& b) {
    for (size_t i = 0; i < a.size(); i++) if (int d = compare(a[i], b[i])) return d;
    return 0;
}
inline bool before(const Row& a, const Row& b) { return compareRow(a, b) < 0; }
inline bool same(const Row& a, const Row& b) { return compareRow(a, b) == 0; }
// sort, then drop adjacent duplicates (stable so the first of 1 and 1.0 is kept)
inline void normalize(Rel& r) {
    std::stable_sort(r.rows.begin(), r.rows.end(), before);
    std::vector<Row> out;
    for (Row& x : r.rows) if (out.empty() || !same(out.back(), x)) out.push_back(std::move(x));
    r.rows = std::move(out);
}

// schema helpers
inline void checkUnique(const std::vector<Col>& cols, int line, int col, const std::string& what) {
    for (size_t i = 0; i < cols.size(); i++)
        for (size_t j = 0; j < i; j++)
            if (cols[i].name == cols[j].name && cols[i].rel == cols[j].rel)
                throw Error{"schema error: duplicate attribute " + cols[i].rel + "." + cols[i].name + " in " + what, line, col};
}
// Name or Rel.Name has to match exactly one column
inline size_t resolve(const std::vector<Col>& cols, Node& a) {
    size_t dot = a.val.text.find('.'), hit = cols.size();
    std::string rel = dot == std::string::npos ? "" : a.val.text.substr(0, dot);
    std::string name = dot == std::string::npos ? a.val.text : a.val.text.substr(dot + 1);
    for (size_t i = 0; i < cols.size(); i++)
        if (cols[i].name == name && (rel.empty() || cols[i].rel == rel)) {
            if (hit < cols.size()) throw Error{"name error: ambiguous attribute " + a.val.text, a.line, a.col};
            hit = i;
        }
    if (hit == cols.size()) throw Error{"name error: unknown attribute " + a.val.text, a.line, a.col};
    a.idx = hit;
    return hit;
}

// resolves attributes once and type checks comparisons before running
inline std::string desc(const Node& n) { return n.kind == StrLit ? quoted(n.val.text) : n.val.text; }
inline Type bindCond(Node& c, const std::vector<Col>& cols) {
    if (c.kind == Attr) return cols[resolve(cols, c)].type;
    if (c.kind == NumLit) return NumT;
    if (c.kind == StrLit) return StrT;
    Type l = bindCond(c.kids[0], cols), r = c.kids.size() > 1 ? bindCond(c.kids[1], cols) : Any;
    if (l != Any && r != Any && l != r)
        throw Error{"type error: cannot compare " + std::string(TYPE[l]) + " " + desc(c.kids[0]) + " with " + TYPE[r] + " " + desc(c.kids[1]), c.line, c.col};
    return Any;
}
// select and join both use this, join passes the right row too
inline const Value& valueOf(const Node& n, const Row& l, const Row& r) {
    return n.kind != Attr ? n.val : n.idx < l.size() ? l[n.idx] : r[n.idx - l.size()];
}
inline bool eval(const Node& c, const Row& l, const Row& r) {
    switch (c.kind) {
        case And: return eval(c.kids[0], l, r) && eval(c.kids[1], l, r);
        case Or: return eval(c.kids[0], l, r) || eval(c.kids[1], l, r);
        case Not: return !eval(c.kids[0], l, r);
        default: break;
    }
    int d = compare(valueOf(c.kids[0], l, r), valueOf(c.kids[1], l, r));
    switch (c.kind) { case Eq: return d == 0; case Ne: return d != 0; case Lt: return d < 0; case Le: return d <= 0; case Gt: return d > 0; default: return d >= 0; }
}

// runs the tree bottom up
struct Engine {
    std::map<std::string, Rel> db;
    unsigned long long comparisons = 0;

    Rel exec(Node& n) {
        switch (n.kind) {
            case Relation: { auto it = db.find(n.val.text); if (it == db.end()) throw Error{"name error: unknown relation " + n.val.text, n.line, n.col}; return it->second; }
            case Select: return select(n, exec(n.kids[1]));
            case Project: return project(n, exec(n.kids[1]));
            case Rename: return rename(n, exec(n.kids[1]));
            default: { Rel a = exec(n.kids[n.kids.size() - 2]), b = exec(n.kids.back()); return n.kind == Times || n.kind == Join ? product(n, a, b) : setop(n, a, b); }
        }
    }
    Rel select(Node& op, Rel in) {
        bindCond(op.kids[0], in.cols);
        Rel out{in.cols, {}};
        Row none;
        for (Row& r : in.rows) { comparisons++; if (eval(op.kids[0], r, none)) out.rows.push_back(std::move(r)); }
        return out;
    }
    Rel project(Node& op, const Rel& in) {
        Rel out;
        for (Node& a : op.kids[0].kids) out.cols.push_back(in.cols[resolve(in.cols, a)]);
        checkUnique(out.cols, op.line, op.col, "project");
        for (const Row& r : in.rows) { Row o; for (const Node& a : op.kids[0].kids) o.push_back(r[a.idx]); out.rows.push_back(std::move(o)); }
        normalize(out);
        return out;
    }
    Rel rename(const Node& op, Rel in) {
        for (Col& c : in.cols) c.rel = op.kids[0].val.text;
        checkUnique(in.cols, op.line, op.col, "rename");
        return in;
    }
    // times, and join without building the full product first
    Rel product(Node& op, const Rel& a, const Rel& b) {
        Rel out{a.cols, {}};
        out.cols.insert(out.cols.end(), b.cols.begin(), b.cols.end());
        checkUnique(out.cols, op.line, op.col, op.val.text);
        Node* c = op.kind == Join ? &op.kids[0] : nullptr;
        if (c) bindCond(*c, out.cols);
        for (const Row& x : a.rows)
            for (const Row& y : b.rows) {
                if (c) { comparisons++; if (!eval(*c, x, y)) continue; }
                out.rows.push_back(x);
                out.rows.back().insert(out.rows.back().end(), y.begin(), y.end());
            }
        return out;
    }
    // union, intersect, minus
    Rel setop(const Node& op, const Rel& a, const Rel& b) {
        std::string bad = a.cols.size() != b.cols.size() ? "different number of attributes" : "";
        for (size_t i = 0; bad.empty() && i < a.cols.size(); i++)
            if (a.cols[i].name != b.cols[i].name) bad = "attribute " + std::to_string(i + 1) + " is " + a.cols[i].name + " on the left and " + b.cols[i].name + " on the right";
            else if (a.cols[i].type != Any && b.cols[i].type != Any && a.cols[i].type != b.cols[i].type) bad = "attribute " + a.cols[i].name + " has different types";
        if (!bad.empty()) throw Error{"schema error: " + op.val.text + " inputs are not union compatible: " + bad, op.line, op.col};
        Rel out{a.cols, {}};
        for (size_t i = 0; i < a.cols.size(); i++) if (out.cols[i].type == Any) out.cols[i].type = b.cols[i].type;
        if (op.kind == Union) { out.rows = a.rows; out.rows.insert(out.rows.end(), b.rows.begin(), b.rows.end()); normalize(out); return out; }
        for (const Row& x : a.rows) {
            bool found = std::any_of(b.rows.begin(), b.rows.end(), [&](const Row& y) { return same(x, y); });
            if (found == (op.kind == Intersect)) out.rows.push_back(x);
        }
        return out;
    }
};

// prints the header then one tuple per line
inline bool bare(const std::string& s) {
    return !s.empty() && numberEnd(s, 0) != s.size() && s.find_first_of(",()' \t\r") == std::string::npos && s.compare(0, 2, "//") != 0 && s != "}";
}
inline std::string show(const Value& v) { return v.isNum || bare(v.text) ? v.text : quoted(v.text); }
inline void print(const Rel& r, std::ostream& out) {
    bool plain = std::all_of(r.cols.begin(), r.cols.end(), [&](const Col& c) { return c.rel == r.cols[0].rel; });
    for (size_t i = 0; i < r.cols.size(); i++) out << (i ? ", " : "") << (plain ? "" : r.cols[i].rel + ".") << r.cols[i].name;
    out << '\n';
    for (const Row& row : r.rows) { for (size_t i = 0; i < row.size(); i++) out << (i ? ", " : "") << show(row[i]); out << '\n'; }
}

// data file loading
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
            normalize(cur); db[name] = std::move(cur); cur = {}; name.clear();
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
                Type t = row[j].isNum ? NumT : StrT;
                if (cur.cols[j].type != Any && cur.cols[j].type != t) throw Error{"type error: attribute " + cur.cols[j].name + " of " + name + " mixes numbers and strings", ln, 1};
                cur.cols[j].type = t;
            }
            cur.rows.push_back(std::move(row));
        }
    }
    if (!name.empty()) throw Error{"syntax error: missing '}' for " + name, ln, 1};
    return db;
}
