// relations as bags, binding, condition evaluation and the operators
#pragma once
#include "parser.hpp"
#include <functional>

// BigT is a 64 bit integer, only ever the result of SUM
enum Type { Any, IntT, RealT, StrT, BigT };
const char* const TYPE[] = {"any", "number", "number", "string", "number"};
inline bool comparable(Type a, Type b) { return a == Any || b == Any || (a == StrT) == (b == StrT); }
// length is the n of VARCHAR(n), 0 for the other types
struct Col { std::string rel, name; Type type; int length = 0; };
inline std::string qualified(const Col& c) { return c.rel.empty() ? c.name : c.rel + "." + c.name; }
inline std::string typeName(const Col& c) { return c.type == StrT ? "VARCHAR(" + std::to_string(c.length) + ")" : c.type == RealT ? "REAL" : c.type == BigT ? "BIGINT" : "INT"; }
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
// stable, so of two equal tuples like 1 and 1.0 the first one stays first
inline void sortRows(std::vector<Row>& rows) { std::stable_sort(rows.begin(), rows.end(), before); }
inline void removeDuplicates(Rel& r) {
    sortRows(r.rows);
    std::vector<Row> out;
    for (Row& x : r.rows) if (out.empty() || !same(out.back(), x)) out.push_back(std::move(x));
    r.rows = std::move(out);
}

// schema helpers
inline void checkUnique(const std::vector<Col>& cols, int line, int col, const std::string& what) {
    for (size_t i = 0; i < cols.size(); i++)
        for (size_t j = 0; j < i; j++)
            if (cols[i].name == cols[j].name && cols[i].rel == cols[j].rel)
                throw Error{"schema error: duplicate attribute " + qualified(cols[i]) + " in " + what, line, col};
}
// Name or Rel.Name has to match exactly one column
inline size_t resolve(const std::vector<Col>& cols, Node& a) {
    size_t dot = a.val.text.find('.'), hit = cols.size();
    std::string rel = dot == std::string::npos ? "" : a.val.text.substr(0, dot);
    std::string name = dot == std::string::npos ? a.val.text : a.val.text.substr(dot + 1);
    for (size_t i = 0; i < cols.size(); i++)
        if (cols[i].name == name && (dot == std::string::npos || cols[i].rel == rel)) {
            if (hit < cols.size()) throw Error{"name error: ambiguous attribute " + a.val.text, a.line, a.col};
            hit = i;
        }
    if (hit == cols.size()) throw Error{"name error: unknown attribute " + a.val.text, a.line, a.col};
    a.idx = hit;
    return hit;
}

// resolves attributes once and type checks comparisons before running
inline Type bindCond(Node& c, const std::vector<Col>& cols) {
    if (c.kind == Attr) return cols[resolve(cols, c)].type;
    if (c.kind == NumLit) return RealT;
    if (c.kind == StrLit) return StrT;
    Type l = bindCond(c.kids[0], cols), r = c.kids.size() > 1 ? bindCond(c.kids[1], cols) : Any;
    if (!comparable(l, r))
        throw Error{"type error: cannot compare " + std::string(TYPE[l]) + " " + operandText(c.kids[0]) + " with " + TYPE[r] + " " + operandText(c.kids[1]), c.line, c.col};
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
inline bool eval(const Node& c, const Row& r) { static const Row none; return eval(c, r, none); }

// runs the tree bottom up on bags; the relations come from whatever source it is given
struct Engine {
    std::function<const Rel*(const std::string&)> source;
    unsigned long long comparisons = 0;

    Rel exec(Node& n) {
        switch (n.kind) {
            case Relation: { const Rel* r = source(n.val.text); if (!r) throw Error{"name error: unknown relation " + n.val.text, n.line, n.col}; return *r; }
            case Distinct: { Rel r = exec(n.kids[0]); removeDuplicates(r); return r; }
            case Select: return select(n, exec(n.kids[1]));
            case Project: return project(n, exec(n.kids[1]));
            case Rename: return rename(n, exec(n.kids[1]));
            default: { Rel a = exec(n.kids[n.kids.size() - 2]), b = exec(n.kids.back()); return n.kind == Times || n.kind == Join ? product(n, a, b) : setop(n, a, b); }
        }
    }
    Rel select(Node& op, Rel in) {
        bindCond(op.kids[0], in.cols);
        Rel out{in.cols, {}};
        for (Row& r : in.rows) { comparisons++; if (eval(op.kids[0], r)) out.rows.push_back(std::move(r)); }
        return out;
    }
    // keeps duplicates, distinct removes them
    Rel project(Node& op, const Rel& in) {
        Rel out;
        for (Node& a : op.kids[0].kids) out.cols.push_back(in.cols[resolve(in.cols, a)]);
        checkUnique(out.cols, op.line, op.col, "project");
        for (const Row& r : in.rows) { Row o; for (const Node& a : op.kids[0].kids) o.push_back(r[a.idx]); out.rows.push_back(std::move(o)); }
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
    // union, intersect, minus, all with set semantics
    Rel setop(const Node& op, const Rel& a, const Rel& b) {
        std::string bad = a.cols.size() != b.cols.size() ? "different number of attributes" : "";
        for (size_t i = 0; bad.empty() && i < a.cols.size(); i++)
            if (a.cols[i].name != b.cols[i].name) bad = "attribute " + std::to_string(i + 1) + " is " + a.cols[i].name + " on the left and " + b.cols[i].name + " on the right";
            else if (!comparable(a.cols[i].type, b.cols[i].type)) bad = "attribute " + a.cols[i].name + " has different types";
        if (!bad.empty()) throw Error{"schema error: " + op.val.text + " inputs are not union compatible: " + bad, op.line, op.col};
        Rel out{a.cols, {}};
        for (size_t i = 0; i < a.cols.size(); i++) if (out.cols[i].type == Any) out.cols[i].type = b.cols[i].type;
        if (op.kind == Union) { out.rows = a.rows; out.rows.insert(out.rows.end(), b.rows.begin(), b.rows.end()); removeDuplicates(out); return out; }
        for (const Row& x : a.rows) {
            bool found = std::any_of(b.rows.begin(), b.rows.end(), [&](const Row& y) { return same(x, y); });
            if (found == (op.kind == Intersect)) out.rows.push_back(x);
        }
        removeDuplicates(out);
        return out;
    }
};
