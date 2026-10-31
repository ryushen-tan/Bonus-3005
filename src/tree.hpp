// parse tree nodes, the tree printer and the condition printer
#pragma once
#include "lexer.hpp"
#include <algorithm>
#include <ostream>
#include <utility>

struct Value { bool isNum; double num; std::string text; };

// Select to Join keep their parameter in kids[0]
enum Kind { Relation, Union, Intersect, Minus, Times, Distinct, Select, Project, Rename, Join, List, And, Or, Not, Eq, Ne, Lt, Le, Gt, Ge, Attr, NumLit, StrLit };
const char* const NAME[] = {"Relation", "Union", "Intersect", "Minus", "Times", "Distinct", "Select", "Project", "Rename", "Join", "List", "And", "Or", "Not", "Eq", "Ne", "Lt", "Le", "Gt", "Ge", "Attr", "Num", "Str"};
const char* const SYMBOL[] = {"", "", "", "", "", "", "", "", "", "", "", "and", "or", "not", "=", "!=", "<", "<=", ">", ">="};

// depth counts nesting, in which a chain of ands or of ors is one level however long; height counts every level
struct Node { Kind kind; Value val; size_t idx = 0; int line = 0, col = 0, depth = 1, height = 1; std::vector<Node> kids; };

const int MAX_DEPTH = 500, MAX_HEIGHT = 2000;

// builds a node, refuses trees nested deeper than MAX_DEPTH or taller than MAX_HEIGHT so that nothing walking
// them, evaluation or Module 4 placing each conjunct of a chain, can overflow the stack
template <class... K> Node mk(Kind k, const Token& t, K... kids) {
    Node n{k, {t.kind == Num, t.kind == Num ? toNum(t.text, t.line, t.col) : 0, t.text}, 0, t.line, t.col, 1, 1, {}};
    (n.kids.push_back(std::move(kids)), ...);
    for (size_t i = 0; i < n.kids.size(); i++) {
        bool chained = i == 0 && (k == And || k == Or) && n.kids[i].kind == k;
        n.depth = std::max(n.depth, n.kids[i].depth + (chained ? 0 : 1));
        n.height = std::max(n.height, n.kids[i].height + 1);
    }
    if (n.depth > MAX_DEPTH) throw Error{"syntax error: nesting too deep", t.line, t.col};
    if (n.height > MAX_HEIGHT) throw Error{"syntax error: condition too long", t.line, t.col};
    return n;
}

inline std::string oneLine(const Node& n) {
    if (n.kind == StrLit) return "Str(" + quoted(n.val.text) + ")";
    if (n.kind == List) { std::string s = "["; for (size_t i = 0; i < n.kids.size(); i++) s += (i ? ", " : "") + n.kids[i].val.text; return s + "]"; }
    if (n.kids.empty()) return std::string(NAME[n.kind]) + "(" + n.val.text + ")";
    std::string s = std::string(NAME[n.kind]) + "(";
    for (size_t i = 0; i < n.kids.size(); i++) s += (i ? ", " : "") + oneLine(n.kids[i]);
    return s + ")";
}
inline std::string label(const Node& n) {
    std::string s = NAME[n.kind];
    if (n.kind == Relation) return s + "(" + n.val.text + ")";
    if (n.kind == Rename) return s + "(name=" + n.kids[0].val.text + ")";
    if (n.kind == Project) return s + "(attrs=" + oneLine(n.kids[0]) + ")";
    if (n.kind == Select || n.kind == Join) return s + "(cond=" + oneLine(n.kids[0]) + ")";
    return s;
}
inline void printTree(const Node& n, std::ostream& out, const std::string& prefix = "") {
    out << label(n) << '\n';
    for (size_t i = n.kind >= Select && n.kind <= Join ? 1 : 0; i < n.kids.size(); i++) {
        bool last = i + 1 == n.kids.size();
        out << prefix << (last ? "└── " : "├── ");
        printTree(n.kids[i], out, prefix + (last ? "    " : "│   "));
    }
}

// a condition written back in the query language, parenthesised only where the grammar needs it
inline int precedence(Kind k) { return k == Or ? 1 : k == And ? 2 : k == Not ? 3 : 4; }
inline std::string operandText(const Node& n) { return n.kind == StrLit ? quoted(n.val.text) : n.val.text; }
inline std::string condText(const Node& c, int outer = 0) {
    std::string s;
    if (c.kind == Or || c.kind == And) s = condText(c.kids[0], precedence(c.kind)) + " " + SYMBOL[c.kind] + " " + condText(c.kids[1], precedence(c.kind) + 1);
    else if (c.kind == Not) s = "not " + condText(c.kids[0], precedence(Not));
    else s = operandText(c.kids[0]) + " " + SYMBOL[c.kind] + " " + operandText(c.kids[1]);
    return precedence(c.kind) < outer ? "(" + s + ")" : s;
}
