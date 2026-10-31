// Module 3, the syntax half: a hand-written SQL scanner, the syntax tree, a recursive descent parser and
// a printer for the tree
#pragma once
#include "lexer.hpp"
#include <algorithm>
#include <memory>
#include <ostream>
#include <set>

// errors of the SQL front end carry a category: lexical, syntax, name, ambiguity, type, aggregate, schema
struct SqlError { std::string category, message; int line, col; };
inline std::string sqlErrorText(const SqlError& e) { return "error: " + e.category + ": " + e.message + " at line " + std::to_string(e.line) + ", column " + std::to_string(e.col); }

enum SqlTok { KeywordToken, IdentifierToken, IntegerToken, StringToken, OperatorToken, PunctToken, EndToken };
struct SqlToken { SqlTok kind; std::string text; int line, col; };

const std::set<std::string> KEYWORDS = {"SELECT", "DISTINCT", "FROM", "WHERE", "GROUP", "BY", "HAVING", "ORDER", "ASC", "DESC", "LIMIT", "UNION", "ALL",
                                        "INTERSECT", "EXCEPT", "JOIN", "INNER", "ON", "AS", "AND", "OR", "NOT", "CREATE", "TABLE", "INSERT", "INTO",
                                        "VALUES", "INT", "VARCHAR", "COUNT", "SUM", "AVG", "MIN", "MAX"};

inline std::string upper(std::string s) { for (char& c : s) c = char(std::toupper((unsigned char)c)); return s; }

// keywords in any case come out upper case; identifiers keep their case
inline std::vector<SqlToken> sqlTokens(const std::string& s) {
    if (size_t bad = invalidUtf8At(s); bad != std::string::npos) {
        size_t lineStart = s.rfind('\n', bad);
        int badLine = 1 + int(std::count(s.begin(), s.begin() + long(bad), '\n'));
        int badCol = int(bad - (lineStart == std::string::npos ? 0 : lineStart + 1)) + 1;
        throw SqlError{"lexical", "the text is not valid UTF-8", badLine, badCol};
    }
    std::vector<SqlToken> out;
    int line = 1;
    size_t start = 0;
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        int col = int(i - start) + 1;
        auto emit = [&](SqlTok k, const std::string& t) { out.push_back({k, t, line, col}); };
        if (c == '\n') { line++; start = ++i; }
        else if (c == ' ' || c == '\t' || c == '\r') i++;
        else if (s.compare(i, 2, "--") == 0) while (i < s.size() && s[i] != '\n') i++;
        else if (wordChar(c) && !dig(c)) {
            size_t j = i;
            while (j < s.size() && wordChar(s[j])) j++;
            std::string word = s.substr(i, j - i);
            if (KEYWORDS.count(upper(word))) emit(KeywordToken, upper(word)); else emit(IdentifierToken, word);
            i = j;
        }
        else if (dig(c) || ((c == '-' || c == '+') && i + 1 < s.size() && dig(s[i + 1]))) {
            size_t j = i + 1;
            while (j < s.size() && dig(s[j])) j++;
            if (j < s.size() && wordChar(s[j])) throw SqlError{"lexical", "a number cannot run into a name: '" + s.substr(i, j - i + 1) + "'", line, col};
            size_t numberStart = c == '+' ? i + 1 : i;
            emit(IntegerToken, s.substr(numberStart, j - numberStart));
            i = j;
        }
        else if (c == '\'') {
            try { emit(StringToken, scanString(s, i, line, col)); } catch (const Error&) { throw SqlError{"lexical", "unterminated string", line, col}; }
        }
        else if (c == '<' || c == '>' || c == '!' || c == '=') {
            size_t len = i + 1 < s.size() && (s[i + 1] == '=' || (c == '<' && s[i + 1] == '>')) && c != '=' ? 2 : 1;
            std::string op = s.substr(i, len);
            if (op == "!") throw SqlError{"lexical", "unexpected character '!'", line, col};
            emit(OperatorToken, op == "!=" ? "<>" : op);
            i += len;
        }
        else if (std::string("(),.;*").find(c) != std::string::npos) { emit(PunctToken, std::string(1, c)); i++; }
        else {
            size_t j = i + 1;
            while (j < s.size() && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
            throw SqlError{"lexical", "unexpected character '" + s.substr(i, j - i) + "'", line, col};
        }
    }
    out.push_back({EndToken, "end of input", line, int(s.size() - start) + 1});
    return out;
}

// ---- the syntax tree ----

struct ColumnRef { std::string qualifier, name; int line = 0, col = 0; };
inline std::string refText(const ColumnRef& c) { return c.qualifier.empty() ? c.name : c.qualifier + "." + c.name; }
struct AggCall { std::string func; bool star = false; ColumnRef column; int line = 0, col = 0; };
inline std::string aggText(const AggCall& a) { return upper(a.func) + "(" + (a.star ? "*" : refText(a.column)) + ")"; }
struct SqlOperand { enum { Column, Int, Str, Agg } kind; ColumnRef column; std::string literal; AggCall agg; int line = 0, col = 0; };
// an Or or And holds every operand of its chain, so a long chain is one wide node and not a deep one
struct Condition {
    enum { Or, And, Not, Compare } kind;
    std::string op;
    std::vector<Condition> kids;
    SqlOperand left, right;
    int line = 0, col = 0;
};
struct SelectItem { bool isAgg = false; ColumnRef column; AggCall agg; std::string alias; int line = 0, col = 0; };
struct OrderItem { bool isAgg = false; ColumnRef column; AggCall agg; bool desc = false; int line = 0, col = 0; };
struct Query;
struct TableRef { std::string table; std::shared_ptr<Query> sub; std::string alias; int line = 0, col = 0; };
// one chain of JOIN ... ON; the chains of a FROM clause are separated by commas
struct JoinChain { std::vector<TableRef> refs; std::vector<Condition> on; };
struct SelectBlock {
    bool distinct = false, star = false;
    std::vector<SelectItem> items;
    std::vector<JoinChain> from;
    std::unique_ptr<Condition> where, having;
    std::vector<ColumnRef> groupBy;
    bool grouped = false;
    int line = 0, col = 0;
};
struct QueryTerm { std::shared_ptr<SelectBlock> block; std::shared_ptr<Query> paren; };
struct Query {
    std::vector<QueryTerm> terms;
    std::vector<std::string> ops;
    std::vector<OrderItem> orderBy;
    long long limit = -1;
    int line = 0, col = 0;
};
struct ColumnDef { std::string name; bool varchar = false; int length = 0; int line = 0, col = 0; };
struct Statement {
    enum { Create, Insert, Select } kind;
    std::string table;
    // where CREATE TABLE names its table
    int nameLine = 0, nameCol = 0;
    std::vector<ColumnDef> columns;
    std::vector<SqlOperand> values;
    std::shared_ptr<Query> query;
    int line = 0, col = 0;
};

// ---- the parser, one method per rule of GRAMMAR_SQL.md ----

// the most a statement may hold, so that the recursive parts of the system stay within the stack: levels of
// parentheses, NOT and subqueries together, comparisons in one condition, and set operators. The optimizer
// orders the tables of a FROM clause by trying every subset of them, so the tables are limited too
const int MAX_NESTING = 450, MAX_COMPARISONS = 1000, MAX_SET_OPERATORS = 1000, MAX_TABLES = 16;
// The deepest plan those limits allow, the optimizer's rewrites included (GRAMMAR_SQL.md section 5). A path from
// the root meets each set operator at most once, and at most MAX_NESTING + 1 query blocks, nested in each other.
// A block adds at most three levels per table, a join, the projection rule 6 puts on its input and a filter split
// off a condition too tall to be one, and BLOCK_LEVELS others. With rule 7 off every conjunct is a filter of its
// own, and room is left for the conjuncts of one block's WHERE, HAVING and ON conditions
const int BLOCK_LEVELS = 13;
const int MAX_PLAN_DEPTH = MAX_SET_OPERATORS + (MAX_NESTING + 1) * (BLOCK_LEVELS + 3 * MAX_TABLES) + (MAX_TABLES + 1) * MAX_COMPARISONS;
// a plan nests one object per node, a node at most two arrays (a sort's keys), and sql compile writes the plans of
// a script with several statements in one more array
const int MAX_JSON_DEPTH = MAX_PLAN_DEPTH + 3;

struct SqlParser {
    std::vector<SqlToken> toks;
    size_t p = 0;
    int depth = 0, comparisons = 0, setOperators = 0;
    const SqlToken& cur() const { return toks[p]; }
    SqlToken take() { return toks[p++]; }
    bool at(const std::string& text) const { return (cur().kind == KeywordToken || cur().kind == PunctToken || cur().kind == OperatorToken) && cur().text == text; }
    bool accept(const std::string& text) { if (!at(text)) return false; p++; return true; }
    static std::string shown(const SqlToken& t) {
        switch (t.kind) {
            case EndToken: return t.text;
            case StringToken: return "string " + quoted(t.text);
            case KeywordToken: return "keyword " + t.text;
            default: return "'" + t.text + "'";
        }
    }
    [[noreturn]] void fail(const std::string& what) { throw SqlError{"syntax", "expected " + what + " but found " + shown(cur()), cur().line, cur().col}; }
    SqlToken expect(const std::string& text) { if (!at(text)) fail(text.size() == 1 ? "'" + text + "'" : text); return take(); }
    SqlToken ident(const std::string& what) { if (cur().kind != IdentifierToken) fail(what); return take(); }
    void enter() { if (++depth > MAX_NESTING) throw SqlError{"syntax", "nesting too deep, at most " + std::to_string(MAX_NESTING) + " levels of parentheses, NOT and subqueries", cur().line, cur().col}; }

    std::vector<Statement> script() {
        std::vector<Statement> out;
        while (cur().kind != EndToken) {
            if (accept(";")) continue;
            out.push_back(statement());
            if (cur().kind != EndToken) expect(";");
        }
        return out;
    }
    Statement statement() {
        Statement s;
        s.line = cur().line; s.col = cur().col;
        setOperators = 0;
        if (accept("CREATE")) {
            s.kind = Statement::Create;
            expect("TABLE");
            SqlToken name = ident("a table name");
            s.table = name.text; s.nameLine = name.line; s.nameCol = name.col;
            expect("(");
            do s.columns.push_back(columnDef()); while (accept(","));
            expect(")");
        } else if (accept("INSERT")) {
            s.kind = Statement::Insert;
            expect("INTO");
            s.table = ident("a table name").text;
            expect("VALUES");
            expect("(");
            do {
                if (cur().kind != IntegerToken && cur().kind != StringToken) fail("a number or a string");
                s.values.push_back(operand());
            } while (accept(","));
            expect(")");
        } else if (at("SELECT") || at("(")) {
            s.kind = Statement::Select;
            s.query = query();
        } else fail("SELECT, CREATE or INSERT");
        return s;
    }
    ColumnDef columnDef() {
        ColumnDef c;
        SqlToken name = ident("a column name");
        c.name = name.text; c.line = name.line; c.col = name.col;
        if (accept("INT")) return c;
        expect("VARCHAR");
        expect("(");
        if (cur().kind != IntegerToken) fail("a length");
        SqlToken n = take();
        if (n.text.size() > 4 || n.text[0] == '-' || std::stoi(n.text) < 1 || std::stoi(n.text) > 255) throw SqlError{"schema", "VARCHAR length must be 1 to 255, not " + n.text, n.line, n.col};
        c.varchar = true;
        c.length = std::stoi(n.text);
        expect(")");
        return c;
    }
    // set operators are all left associative at one level of precedence; ORDER BY and LIMIT after the
    // last term apply to the whole query, as in PostgreSQL. A parenthesised query on its own is that query, so
    // ORDER BY and LIMIT after it apply as if written inside the parentheses, and only one of each may be written
    std::shared_ptr<Query> query() {
        auto q = std::make_shared<Query>();
        q->line = cur().line; q->col = cur().col;
        q->terms.push_back(term());
        for (;;) {
            SqlToken t = cur();
            std::string op;
            if (accept("UNION")) op = accept("ALL") ? "union_all" : "union";
            else if (accept("INTERSECT")) op = "intersect";
            else if (accept("EXCEPT")) op = "except";
            else break;
            if (++setOperators > MAX_SET_OPERATORS) throw SqlError{"syntax", "a statement may hold at most " + std::to_string(MAX_SET_OPERATORS) + " set operators", t.line, t.col};
            q->ops.push_back(op);
            q->terms.push_back(term());
        }
        if (q->terms.size() == 1 && q->terms[0].paren) q = q->terms[0].paren;
        if (at("ORDER")) {
            SqlToken order = take();
            if (!q->orderBy.empty()) throw SqlError{"syntax", "multiple ORDER BY clauses are not allowed", order.line, order.col};
            expect("BY");
            do {
                OrderItem o;
                o.line = cur().line; o.col = cur().col;
                if (aggregateStart()) { o.isAgg = true; o.agg = aggregate(); } else o.column = column();
                if (accept("DESC")) o.desc = true; else accept("ASC");
                q->orderBy.push_back(o);
            } while (accept(","));
        }
        if (at("LIMIT")) {
            SqlToken limit = take();
            if (q->limit >= 0) throw SqlError{"syntax", "multiple LIMIT clauses are not allowed", limit.line, limit.col};
            if (cur().kind != IntegerToken || cur().text[0] == '-' || cur().text.size() > 18) fail("a non-negative number of rows");
            q->limit = std::stoll(take().text);
        }
        return q;
    }
    QueryTerm term() {
        QueryTerm t;
        if (accept("(")) { enter(); t.paren = query(); expect(")"); depth--; return t; }
        t.block = selectBlock();
        return t;
    }
    std::shared_ptr<SelectBlock> selectBlock() {
        auto b = std::make_shared<SelectBlock>();
        SqlToken s = expect("SELECT");
        b->line = s.line; b->col = s.col;
        b->distinct = accept("DISTINCT");
        if (accept("*")) b->star = true;
        else do b->items.push_back(selectItem()); while (accept(","));
        expect("FROM");
        int tables = 0;
        do b->from.push_back(joinChain(tables)); while (accept(","));
        if (accept("WHERE")) b->where = std::make_unique<Condition>(clause());
        if (accept("GROUP")) {
            expect("BY");
            b->grouped = true;
            do b->groupBy.push_back(column()); while (accept(","));
            if (accept("HAVING")) b->having = std::make_unique<Condition>(clause());
        }
        return b;
    }
    bool aggregateStart() const { return at("COUNT") || at("SUM") || at("AVG") || at("MIN") || at("MAX"); }
    SelectItem selectItem() {
        SelectItem item;
        item.line = cur().line; item.col = cur().col;
        if (aggregateStart()) { item.isAgg = true; item.agg = aggregate(); } else item.column = column();
        if (accept("AS")) item.alias = ident("an alias").text;
        else if (cur().kind == IdentifierToken) item.alias = take().text;
        return item;
    }
    AggCall aggregate() {
        AggCall a;
        SqlToken f = take();
        a.func = f.text == "COUNT" ? "count" : f.text == "SUM" ? "sum" : f.text == "AVG" ? "avg" : f.text == "MIN" ? "min" : "max";
        a.line = f.line; a.col = f.col;
        expect("(");
        if (a.func == "count" && accept("*")) a.star = true;
        else a.column = column();
        expect(")");
        return a;
    }
    ColumnRef column() {
        SqlToken first = ident("a column name");
        ColumnRef c{"", first.text, first.line, first.col};
        if (accept(".")) { c.qualifier = first.text; c.name = ident("a column name").text; }
        return c;
    }
    // tables counts the table references of the FROM clause so far
    JoinChain joinChain(int& tables) {
        JoinChain chain;
        chain.refs.push_back(tableRef(tables));
        for (;;) {
            if (accept("INNER")) expect("JOIN");
            else if (!accept("JOIN")) return chain;
            chain.refs.push_back(tableRef(tables));
            expect("ON");
            chain.on.push_back(clause());
        }
    }
    TableRef tableRef(int& tables) {
        if (++tables > MAX_TABLES) throw SqlError{"syntax", "a FROM clause may name at most " + std::to_string(MAX_TABLES) + " tables", cur().line, cur().col};
        TableRef r;
        r.line = cur().line; r.col = cur().col;
        if (accept("(")) {
            enter();
            r.sub = query();
            expect(")");
            depth--;
            accept("AS");
            r.alias = ident("an alias, which a subquery in FROM must have").text;
            return r;
        }
        r.table = ident("a table name").text;
        if (accept("AS")) r.alias = ident("an alias").text;
        else if (cur().kind == IdentifierToken) r.alias = take().text;
        return r;
    }
    // a whole WHERE, HAVING or ON condition
    Condition clause() { comparisons = 0; return condition(); }
    // NOT binds tighter than AND, AND tighter than OR, both left associative
    Condition condition() {
        Condition first = conjunction();
        if (!at("OR")) return first;
        Condition chain{Condition::Or, "", {}, {}, {}, cur().line, cur().col};
        chain.kids.push_back(std::move(first));
        while (accept("OR")) chain.kids.push_back(conjunction());
        return chain;
    }
    Condition conjunction() {
        Condition first = negation();
        if (!at("AND")) return first;
        Condition chain{Condition::And, "", {}, {}, {}, cur().line, cur().col};
        chain.kids.push_back(std::move(first));
        while (accept("AND")) chain.kids.push_back(negation());
        return chain;
    }
    Condition negation() {
        if (at("NOT")) {
            SqlToken t = take();
            enter();
            Condition n{Condition::Not, "", {}, {}, {}, t.line, t.col};
            n.kids.push_back(negation());
            depth--;
            return n;
        }
        if (accept("(")) { enter(); Condition c = condition(); expect(")"); depth--; return c; }
        Condition c{Condition::Compare, "", {}, operand(), {}, 0, 0};
        if (cur().kind != OperatorToken) fail("a comparison operator");
        SqlToken op = take();
        if (++comparisons > MAX_COMPARISONS) throw SqlError{"syntax", "a condition may hold at most " + std::to_string(MAX_COMPARISONS) + " comparisons", op.line, op.col};
        c.op = op.text; c.line = op.line; c.col = op.col;
        c.right = operand();
        return c;
    }
    SqlOperand operand() {
        SqlOperand o;
        o.line = cur().line; o.col = cur().col;
        if (cur().kind == IntegerToken) { o.kind = SqlOperand::Int; o.literal = take().text; }
        else if (cur().kind == StringToken) { o.kind = SqlOperand::Str; o.literal = take().text; }
        else if (aggregateStart()) { o.kind = SqlOperand::Agg; o.agg = aggregate(); }
        else if (cur().kind == IdentifierToken) { o.kind = SqlOperand::Column; o.column = column(); }
        else fail("a column, a number or a string");
        return o;
    }
};
inline std::vector<Statement> parseSql(const std::string& text) { SqlParser p{sqlTokens(text)}; return p.script(); }

// ---- printing the tree ----

inline std::string operandSql(const SqlOperand& o) {
    switch (o.kind) {
        case SqlOperand::Column: return refText(o.column);
        case SqlOperand::Int: return o.literal;
        case SqlOperand::Str: return quoted(o.literal);
        default: return aggText(o.agg);
    }
}
inline std::string conditionSql(const Condition& c, int outer = 0) {
    int level = c.kind == Condition::Or ? 1 : c.kind == Condition::And ? 2 : c.kind == Condition::Not ? 3 : 4;
    std::string s;
    if (c.kind == Condition::Compare) s = operandSql(c.left) + " " + c.op + " " + operandSql(c.right);
    else if (c.kind == Condition::Not) s = "NOT " + conditionSql(c.kids[0], 3);
    else for (size_t k = 0; k < c.kids.size(); k++) s += (k == 0 ? "" : c.kind == Condition::Or ? " OR " : " AND ") + conditionSql(c.kids[k], level + 1);
    return level < outer ? "(" + s + ")" : s;
}
inline void printQuery(const Query& q, std::ostream& out, int depth);
inline void printTable(const TableRef& r, std::ostream& out, int depth) {
    std::string pad(size_t(2 * depth), ' ');
    if (r.sub) { out << pad << "subquery as " << r.alias << '\n'; printQuery(*r.sub, out, depth + 1); }
    else out << pad << "table " << r.table << (r.alias.empty() ? "" : " as " + r.alias) << '\n';
}
inline void printQuery(const Query& q, std::ostream& out, int depth) {
    std::string pad(size_t(2 * depth), ' ');
    out << pad << "query\n";
    for (size_t t = 0; t < q.terms.size(); t++) {
        if (t) out << pad << "  " << q.ops[t - 1] << '\n';
        const QueryTerm& term = q.terms[t];
        if (term.paren) { printQuery(*term.paren, out, depth + 1); continue; }
        const SelectBlock& b = *term.block;
        out << pad << "  select" << (b.distinct ? " distinct" : "") << '\n';
        if (b.star) out << pad << "    *\n";
        for (const SelectItem& i : b.items)
            out << pad << "    " << (i.isAgg ? "aggregate " + aggText(i.agg) : "column " + refText(i.column)) << (i.alias.empty() ? "" : " as " + i.alias) << '\n';
        out << pad << "  from\n";
        for (const JoinChain& chain : b.from) {
            printTable(chain.refs[0], out, depth + 2);
            for (size_t k = 1; k < chain.refs.size(); k++) {
                out << pad << "    join on " << conditionSql(chain.on[k - 1]) << '\n';
                printTable(chain.refs[k], out, depth + 3);
            }
        }
        if (b.where) out << pad << "  where " << conditionSql(*b.where) << '\n';
        if (b.grouped) {
            out << pad << "  group by";
            for (size_t k = 0; k < b.groupBy.size(); k++) out << (k ? ", " : " ") << refText(b.groupBy[k]);
            out << '\n';
        }
        if (b.having) out << pad << "  having " << conditionSql(*b.having) << '\n';
    }
    if (!q.orderBy.empty()) {
        out << pad << "  order by";
        for (size_t k = 0; k < q.orderBy.size(); k++)
            out << (k ? ", " : " ") << (q.orderBy[k].isAgg ? aggText(q.orderBy[k].agg) : refText(q.orderBy[k].column)) << (q.orderBy[k].desc ? " desc" : " asc");
        out << '\n';
    }
    if (q.limit >= 0) out << pad << "  limit " << q.limit << '\n';
}
inline void printStatement(const Statement& s, std::ostream& out) {
    if (s.kind == Statement::Select) { printQuery(*s.query, out, 0); return; }
    if (s.kind == Statement::Create) {
        out << "create table " << s.table << '\n';
        for (const ColumnDef& c : s.columns) out << "  " << c.name << ' ' << (c.varchar ? "VARCHAR(" + std::to_string(c.length) + ")" : "INT") << '\n';
        return;
    }
    out << "insert into " << s.table << '\n';
    for (const SqlOperand& v : s.values) out << "  " << operandSql(v) << '\n';
}
