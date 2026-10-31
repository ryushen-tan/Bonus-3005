// the recursive descent parser, one method per grammar rule
#pragma once
#include "tree.hpp"

struct Parser {
    std::vector<Token> toks; size_t p = 0; int depth = 0;
    static std::string shown(const Token& t) { return t.kind == End ? t.text : t.kind == Str ? "string " + quoted(t.text) : "'" + t.text + "'"; }
    const Token& cur() { return toks[p]; }
    Token take() { return toks[p++]; }
    bool at(const std::string& s) { return cur().kind != Str && cur().text == s; }
    bool accept(const std::string& s) { if (!at(s)) return false; p++; return true; }
    [[noreturn]] void fail(const std::string& what) { throw Error{"syntax error: expected " + what + " but found " + shown(cur()), cur().line, cur().col}; }
    Token expect(const std::string& s) { if (!at(s)) fail("'" + s + "'"); return take(); }
    Token ident(const std::string& what) { if (cur().kind != Word) fail(what); return take(); }
    void done() { if (cur().kind != End) throw Error{"syntax error: unexpected " + shown(cur()), cur().line, cur().col}; }
    // nesting limit for the recursive rules
    void enter(const Token& t) { if (++depth > 500) throw Error{"syntax error: nesting too deep", t.line, t.col}; }

    Node query() { Node n = expr(); done(); return n; }
    Node expr() { Node n = inter(); while (at("union") || at("minus")) { Token t = take(); n = mk(t.text == "union" ? Union : Minus, t, std::move(n), inter()); } return n; }
    Node inter() { Node n = product(); while (at("intersect")) { Token t = take(); n = mk(Intersect, t, std::move(n), product()); } return n; }
    Node product() {
        Node n = primary();
        for (;;) {
            if (at("times")) { Token t = take(); n = mk(Times, t, std::move(n), primary()); }
            else if (at("join")) { Token t = take(); expect("["); Node c = cond(); expect("]"); n = mk(Join, t, std::move(c), std::move(n), primary()); }
            else return n;
        }
    }
    Node primary() {
        Token t = cur();
        if (accept("select")) return unary(Select, t);
        if (accept("project")) return unary(Project, t);
        if (accept("rename")) return unary(Rename, t);
        if (accept("distinct")) { enter(t); expect("("); Node e = expr(); expect(")"); depth--; return mk(Distinct, t, std::move(e)); }
        if (accept("(")) { enter(t); Node e = expr(); expect(")"); depth--; return e; }
        return mk(Relation, ident("relation name"));
    }
    Node unary(Kind k, const Token& t) {
        enter(t);
        expect("[");
        Node param = k == Select ? cond() : k == Project ? attrList() : mk(Relation, ident("relation name"));
        expect("]"); expect("(");
        Node e = expr();
        expect(")");
        depth--;
        return mk(k, t, std::move(param), std::move(e));
    }
    Node attrList() { Node l = mk(List, cur()); do l.kids.push_back(attr()); while (accept(",")); return l; }
    Node attr() { Token t = ident("attribute name"); if (accept(".")) t.text += "." + ident("attribute name").text; return mk(Attr, t); }
    Node cond() { Node n = andc(); while (at("or")) { Token t = take(); n = mk(Or, t, std::move(n), andc()); } return n; }
    Node andc() { Node n = notc(); while (at("and")) { Token t = take(); n = mk(And, t, std::move(n), notc()); } return n; }
    Node notc() {
        if (!at("not")) return cmp();
        Token t = take();
        enter(t);
        Node n = mk(Not, t, notc());
        depth--;
        return n;
    }
    Node cmp() {
        Token t = cur();
        if (accept("(")) { enter(t); Node c = cond(); expect(")"); depth--; return c; }
        Node l = operand();
        static const std::pair<const char*, Kind> ops[] = {{"=", Eq}, {"!=", Ne}, {"<", Lt}, {"<=", Le}, {">", Gt}, {">=", Ge}};
        for (const auto& [sym, kind] : ops) if (at(sym)) { Token o = take(); return mk(kind, o, std::move(l), operand()); }
        fail("comparison operator");
    }
    Node operand() {
        if (cur().kind == Num || cur().kind == Str) { Token t = take(); return mk(t.kind == Num ? NumLit : StrLit, t); }
        return attr();
    }
};

inline Node parseQuery(const std::string& text) { return Parser{tokenize(text)}.query(); }
// the condition language on its own, as used inside select[...] and join[...]
inline Node parseCondition(const std::string& text) { Parser p{tokenize(text)}; Node c = p.cond(); p.done(); return c; }
