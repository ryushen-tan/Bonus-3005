// Module 3, the meaning half: scopes and name resolution, type and aggregate rules, and translation into
// a logical plan built in the canonical order of section 5 of the module document
#pragma once
#include "algebra.hpp"
#include "logical.hpp"
#include "sqlparse.hpp"
#include <functional>
#include <map>

// one entry per table reference in FROM, named by its alias or its table, with that name on every column;
// for a JOIN's ON condition the entries of the other comma separated items are marked outside
struct ScopeEntry { std::string name; std::vector<Col> cols; bool outside = false; };
struct Compiled { LogicalNode node; std::vector<Col> cols; };

struct AggregateUse { std::string func, column, name; Col out; };
// what an aggregating block groups by and computes, shared by its select list, HAVING and ORDER BY, with the
// select list's aliases, which a default aggregate name must not take
struct Grouping { std::vector<Col> keys; std::vector<AggregateUse> aggs; std::vector<std::string> aliases; };

// a condition's tree is at most MAX_NESTING + 3 levels deep and MAX_COMPARISONS + MAX_NESTING + 3 tall
static_assert(MAX_NESTING + 3 <= MAX_DEPTH && MAX_COMPARISONS + MAX_NESTING + 3 <= MAX_HEIGHT, "mk must accept every condition a statement may hold");

[[noreturn]] inline void sqlFail(const std::string& category, const std::string& message, int line, int col) { throw SqlError{category, message, line, col}; }
inline bool sameColumn(const Col& a, const Col& b) { return a.rel == b.rel && a.name == b.name; }
inline LogicalNode unaryNode(const std::string& op, LogicalNode child) { return logicalNode(op, {std::move(child)}); }
// name, or when that is taken the first of name_2, name_3, ... that is not
inline std::string freeName(const std::string& name, const std::function<bool(const std::string&)>& taken) {
    std::string candidate = name;
    for (int n = 2; taken(candidate); n++) candidate = name + "_" + std::to_string(n);
    return candidate;
}

struct SqlCompiler {
    std::function<const std::vector<Col>*(const std::string&)> tables;
    // what the database's catalog finds wrong with the name of a new table, or nothing
    std::function<std::string(const std::string&)> tableNameProblem;
    std::map<std::string, std::vector<Col>> created;

    const std::vector<Col>* schemaOf(const std::string& name) {
        auto it = created.find(name);
        return it != created.end() ? &it->second : tables(name);
    }

    // t.c needs t in scope and c in t; c alone has to be in exactly one entry
    Col resolveColumn(const ColumnRef& c, const std::vector<ScopeEntry>& scope) {
        if (!c.qualifier.empty()) {
            for (const ScopeEntry& e : scope) {
                if (e.name != c.qualifier) continue;
                if (e.outside) sqlFail("name", c.qualifier + " is not part of this JOIN, so its ON condition cannot use " + refText(c), c.line, c.col);
                for (const Col& col : e.cols) if (col.name == c.name) return col;
                sqlFail("name", "column " + c.name + " does not exist in " + c.qualifier, c.line, c.col);
            }
            sqlFail("name", "unknown table or alias " + c.qualifier + " in " + refText(c), c.line, c.col);
        }
        std::vector<Col> hits;
        for (const ScopeEntry& e : scope) if (!e.outside) for (const Col& col : e.cols) if (col.name == c.name) hits.push_back(col);
        if (hits.empty()) sqlFail("name", "unknown column " + c.name, c.line, c.col);
        if (hits.size() > 1) {
            std::string candidates;
            for (size_t i = 0; i < hits.size(); i++) candidates += (i == 0 ? "" : i + 1 == hits.size() ? " or " : ", ") + qualified(hits[i]);
            sqlFail("ambiguity", "column " + c.name + " is ambiguous, it could be " + candidates, c.line, c.col);
        }
        return hits[0];
    }

    // the aggregate's output column, reusing an identical one already computed unless it is named; a name
    // another aggregate has, or a default name an alias has, gets a suffix
    size_t useAggregate(Grouping& g, const AggCall& a, const std::vector<ScopeEntry>& scope, const std::string& alias) {
        Col out{"", "", IntT};
        std::string column = "*";
        if (!a.star) {
            Col source = resolveColumn(a.column, scope);
            column = qualified(source);
            if ((a.func == "sum" || a.func == "avg") && source.type == StrT)
                sqlFail("type", upper(a.func) + " needs a number, " + column + " is " + typeName(source), a.line, a.col);
            if (a.func == "sum") out.type = source.type == RealT ? RealT : BigT;
            else if (a.func == "avg") out.type = RealT;
            else if (a.func != "count") { out.type = source.type; out.length = source.length; }
        }
        if (alias.empty())
            for (size_t i = 0; i < g.aggs.size(); i++) if (g.aggs[i].func == a.func && g.aggs[i].column == column) return i;
        out.name = freeName(alias.empty() ? aggregateName(a.func, column) : alias, [&](const std::string& name) {
            return std::any_of(g.aggs.begin(), g.aggs.end(), [&](const AggregateUse& u) { return u.name == name; }) ||
                   (alias.empty() && std::count(g.aliases.begin(), g.aliases.end(), name) > 0);
        });
        g.aggs.push_back({a.func, column, out.name, out});
        return g.aggs.size() - 1;
    }
    // a GROUP BY name is a FROM column first and otherwise, as in PostgreSQL, the column a select list alias names
    Col groupKey(const ColumnRef& ref, const std::vector<SelectItem>& items, const std::vector<ScopeEntry>& scope) {
        bool fromColumn = std::any_of(scope.begin(), scope.end(), [&](const ScopeEntry& e) {
            return std::any_of(e.cols.begin(), e.cols.end(), [&](const Col& c) { return c.name == ref.name; });
        });
        if (!ref.qualifier.empty() || fromColumn) return resolveColumn(ref, scope);
        for (const SelectItem& item : items) {
            if (item.alias != ref.name) continue;
            if (item.isAgg) sqlFail("aggregate", "GROUP BY " + ref.name + " names " + aggText(item.agg) + ", and aggregate functions are not allowed in GROUP BY", ref.line, ref.col);
            return resolveColumn(item.column, scope);
        }
        return resolveColumn(ref, scope);
    }
    bool isKey(const Grouping& g, const Col& c) { return std::any_of(g.keys.begin(), g.keys.end(), [&](const Col& k) { return sameColumn(k, c); }); }

    // every integer is an INT, which is 4 bytes here. It is judged by its value, as the loader judges a field, so
    // leading zeros do not count; strtoll saturates past the range of a long long, which is far outside 32 bits
    static long long intLiteral(const SqlOperand& o) {
        long long value = std::strtoll(o.literal.c_str(), nullptr, 10);
        if (value < INT32_MIN || value > INT32_MAX)
            sqlFail("type", o.literal + " does not fit in an INT, which has 4 bytes and holds -2147483648 to 2147483647", o.line, o.col);
        return value;
    }

    // a condition as a Bonus 1 condition tree, which the executor parses back from its text; in HAVING the
    // aggregates become the names of the group_by's output columns
    std::pair<Node, Col> operandNode(const SqlOperand& o, const std::vector<ScopeEntry>& scope, Grouping* having, const std::string& clause) {
        switch (o.kind) {
            case SqlOperand::Int:
                intLiteral(o);
                return {mk(NumLit, Token{Num, o.literal, o.line, o.col}), Col{"", o.literal, IntT}};
            case SqlOperand::Str: return {mk(StrLit, Token{Str, o.literal, o.line, o.col}), Col{"", quoted(o.literal), StrT, int(o.literal.size())}};
            case SqlOperand::Column: {
                Col c = resolveColumn(o.column, scope);
                if (having && !isKey(*having, c)) sqlFail("aggregate", "column " + qualified(c) + " must appear in GROUP BY or be used in an aggregate function", o.line, o.col);
                return {mk(Attr, Token{Word, qualified(c), o.line, o.col}), c};
            }
            default: {
                if (!having) sqlFail("aggregate", "aggregate functions are not allowed in " + clause, o.line, o.col);
                const AggregateUse& u = having->aggs[useAggregate(*having, o.agg, scope, "")];
                return {mk(Attr, Token{Word, u.name, o.line, o.col}), u.out};
            }
        }
    }
    Node conditionNode(const Condition& c, const std::vector<ScopeEntry>& scope, Grouping* having, const std::string& clause) {
        Token t{Word, "", c.line, c.col};
        if (c.kind == Condition::Or || c.kind == Condition::And) {
            Node chain = conditionNode(c.kids[0], scope, having, clause);
            for (size_t k = 1; k < c.kids.size(); k++) chain = mk(c.kind == Condition::Or ? Or : And, t, std::move(chain), conditionNode(c.kids[k], scope, having, clause));
            return chain;
        }
        if (c.kind == Condition::Not) return mk(Not, t, conditionNode(c.kids[0], scope, having, clause));
        auto [l, lt] = operandNode(c.left, scope, having, clause);
        auto [r, rt] = operandNode(c.right, scope, having, clause);
        if (!comparable(lt.type, rt.type))
            sqlFail("type", "cannot compare " + operandSql(c.left) + " (" + typeName(lt) + ") with " + operandSql(c.right) + " (" + typeName(rt) + ")", c.line, c.col);
        static const std::map<std::string, Kind> OPS = {{"=", Eq}, {"<>", Ne}, {"<", Lt}, {"<=", Le}, {">", Gt}, {">=", Ge}};
        return mk(OPS.at(c.op), t, std::move(l), std::move(r));
    }

    LogicalNode tableNode(const TableRef& r, std::vector<ScopeEntry>& scope) {
        std::string name = r.alias.empty() ? r.table : r.alias;
        for (const ScopeEntry& e : scope)
            if (e.name == name) sqlFail("name", "the name " + name + " appears twice in FROM, give one of them an alias", r.line, r.col);
        ScopeEntry entry{name, {}};
        LogicalNode node;
        if (r.sub) {
            // the alias requalifies every column, so a column with an earlier one's name is renamed name_2, name_3, ...
            Compiled c = query(*r.sub);
            std::vector<std::pair<std::string, std::string>> columns;
            for (const Col& col : c.cols) {
                std::string unique = freeName(col.name, [&](const std::string& n) {
                    return std::any_of(entry.cols.begin(), entry.cols.end(), [&](const Col& seen) { return seen.name == n; });
                });
                columns.emplace_back(qualified(col), unique == col.name ? "" : unique);
                entry.cols.push_back({name, unique, col.type, col.length});
            }
            node = std::move(c.node);
            bool renamed = std::any_of(columns.begin(), columns.end(), [](const auto& column) { return !column.second.empty(); });
            if (renamed) { node = unaryNode("project", std::move(node)); node.columns = columns; }
            node = unaryNode("rename", std::move(node));
            node.to = name;
        } else {
            const std::vector<Col>* cols = schemaOf(r.table);
            if (!cols) sqlFail("name", "unknown table " + r.table, r.line, r.col);
            for (const Col& col : *cols) entry.cols.push_back({name, col.name, col.type, col.length});
            node = logicalNode("scan");
            node.table = r.table;
            if (!r.alias.empty()) { node = unaryNode("rename", std::move(node)); node.to = r.alias; }
        }
        scope.push_back(entry);
        return node;
    }

    Compiled block(const SelectBlock& b, const std::vector<OrderItem>& orderBy, long long limit) {
        // 1 FROM: each JOIN chain left to right, the chains combined by cross products; JOIN binds tighter than
        // the comma, so an ON condition sees only the tables of its own chain, as in PostgreSQL
        std::vector<ScopeEntry> scope;
        LogicalNode node;
        for (size_t ci = 0; ci < b.from.size(); ci++) {
            const JoinChain& chain = b.from[ci];
            size_t chainStart = scope.size();
            LogicalNode joined = tableNode(chain.refs[0], scope);
            for (size_t k = 1; k < chain.refs.size(); k++) {
                LogicalNode right = tableNode(chain.refs[k], scope);
                std::vector<ScopeEntry> onScope = scope;
                for (size_t e = 0; e < chainStart; e++) onScope[e].outside = true;
                LogicalNode j = logicalNode("join", {std::move(joined), std::move(right)});
                j.condition = condText(conditionNode(chain.on[k - 1], onScope, nullptr, "JOIN ... ON"));
                joined = std::move(j);
            }
            node = ci == 0 ? std::move(joined) : logicalNode("cross", {std::move(node), std::move(joined)});
        }
        // 2 WHERE, one select carrying the whole condition
        if (b.where) {
            std::string cond = condText(conditionNode(*b.where, scope, nullptr, "WHERE"));
            node = unaryNode("select", std::move(node));
            node.condition = cond;
        }
        // 3 GROUP BY and 4 HAVING
        bool aggregating = b.grouped || std::any_of(b.items.begin(), b.items.end(), [](const SelectItem& i) { return i.isAgg; }) ||
                           std::any_of(orderBy.begin(), orderBy.end(), [](const OrderItem& o) { return o.isAgg; });
        Grouping g;
        std::vector<size_t> itemAggregate(b.items.size()), orderAggregate(orderBy.size());
        if (aggregating) {
            for (const SelectItem& item : b.items) if (!item.alias.empty()) g.aliases.push_back(item.alias);
            for (const ColumnRef& ref : b.groupBy) {
                Col key = groupKey(ref, b.items, scope);
                if (!isKey(g, key)) g.keys.push_back(key);
            }
            for (size_t i = 0; i < b.items.size(); i++) if (b.items[i].isAgg) itemAggregate[i] = useAggregate(g, b.items[i].agg, scope, b.items[i].alias);
            std::string having;
            if (b.having) having = condText(conditionNode(*b.having, scope, &g, "HAVING"));
            for (size_t i = 0; i < orderBy.size(); i++) if (orderBy[i].isAgg) orderAggregate[i] = useAggregate(g, orderBy[i].agg, scope, "");
            node = unaryNode("group_by", std::move(node));
            for (const Col& k : g.keys) node.keys.push_back(qualified(k));
            for (const AggregateUse& u : g.aggs) node.aggregates.push_back({u.func, u.column, u.name});
            if (b.having) { node = unaryNode("select", std::move(node)); node.condition = having; }
        }
        // 5 SELECT: the projection, then distinct if it was asked for and can change anything; a column with an
        // earlier one's qualified name is renamed name_2, name_3, ...
        std::vector<std::pair<std::string, std::string>> columns;
        std::vector<Col> out;
        // each output column's name as the query wrote it, which is what ORDER BY matches
        std::vector<std::string> written;
        auto addColumn = [&](const std::string& source, Col shown, const std::string& name) {
            auto taken = [&](const std::string& n) { return std::any_of(out.begin(), out.end(), [&](const Col& o) { return qualified(o) == n; }); };
            if (taken(qualified(shown))) shown = Col{"", freeName(shown.name, [&](const std::string& n) { return n == shown.name || taken(n); }), shown.type, shown.length};
            columns.emplace_back(source, qualified(shown) == source ? "" : shown.name);
            out.push_back(shown);
            written.push_back(name);
        };
        auto addSelected = [&](const Col& c, const std::string& alias, int line, int col) {
            if (aggregating && !isKey(g, c)) sqlFail("aggregate", "column " + qualified(c) + " must appear in GROUP BY or be used in an aggregate function", line, col);
            addColumn(qualified(c), alias.empty() ? c : Col{"", alias, c.type, c.length}, alias.empty() ? c.name : alias);
        };
        if (b.star) for (const ScopeEntry& e : scope) for (const Col& c : e.cols) addSelected(c, "", b.line, b.col);
        for (size_t i = 0; i < b.items.size(); i++) {
            const SelectItem& item = b.items[i];
            if (!item.isAgg) { addSelected(resolveColumn(item.column, scope), item.alias, item.line, item.col); continue; }
            const AggregateUse& u = g.aggs[itemAggregate[i]];
            addColumn(u.name, u.out, u.name);
        }
        size_t visible = columns.size();
        bool allKeysShown = std::all_of(g.keys.begin(), g.keys.end(), [&](const Col& k) { return std::any_of(out.begin(), out.end(), [&](const Col& o) { return sameColumn(o, k); }); });
        bool distinct = b.distinct && !(aggregating && allKeysShown);
        // 6 ORDER BY: a name alone is first the name of an output column, two of them with different sources
        // being ambiguous, and otherwise a FROM column; a key the select list does not show is carried through
        // the projection and dropped after the sort
        std::vector<std::pair<std::string, bool>> keys;
        for (size_t i = 0; i < orderBy.size(); i++) {
            const OrderItem& o = orderBy[i];
            size_t named = visible;
            for (size_t k = 0; k < visible && !o.isAgg && o.column.qualifier.empty(); k++) {
                if (written[k] != o.column.name) continue;
                if (named < visible && columns[named].first != columns[k].first)
                    sqlFail("ambiguity", "ORDER BY " + o.column.name + " is ambiguous, two columns of the select list are named " + o.column.name, o.line, o.col);
                if (named == visible) named = k;
            }
            if (named < visible) { keys.emplace_back(qualified(out[named]), o.desc); continue; }
            std::string source;
            if (o.isAgg) source = g.aggs[orderAggregate[i]].name;
            else {
                Col c = resolveColumn(o.column, scope);
                if (aggregating && !isKey(g, c)) sqlFail("aggregate", "column " + qualified(c) + " must appear in GROUP BY or be used in an aggregate function", o.line, o.col);
                source = qualified(c);
            }
            size_t shown = 0;
            while (shown < visible && columns[shown].first != source) shown++;
            if (shown < visible) { keys.emplace_back(qualified(out[shown]), o.desc); continue; }
            if (b.distinct) sqlFail("name", "with SELECT DISTINCT, ORDER BY " + (o.isAgg ? aggText(o.agg) : refText(o.column)) + " must be in the select list", o.line, o.col);
            if (std::none_of(columns.begin(), columns.end(), [&](const auto& c) { return c.first == source; })) columns.emplace_back(source, "");
            keys.emplace_back(source, o.desc);
        }
        node = unaryNode("project", std::move(node));
        node.columns = columns;
        if (distinct) node = unaryNode("distinct", std::move(node));
        if (!keys.empty()) { node = unaryNode("sort", std::move(node)); node.sortKeys = keys; }
        if (columns.size() > visible) {
            node = unaryNode("project", std::move(node));
            for (const Col& c : out) node.columns.emplace_back(qualified(c), "");
        }
        // 7 LIMIT
        if (limit >= 0) { node = unaryNode("limit", std::move(node)); node.n = limit; }
        return {std::move(node), out};
    }

    Compiled term(const QueryTerm& t) { return t.paren ? query(*t.paren) : block(*t.block, {}, -1); }
    // 8 the set operators between whole blocks, left associative; then ORDER BY and LIMIT of the whole. The
    // parser has made a parenthesised query on its own into that query, so a query of one term is one block
    Compiled query(const Query& q) {
        if (q.terms.size() == 1) return block(*q.terms[0].block, q.orderBy, q.limit);
        Compiled acc = term(q.terms[0]);
        for (size_t i = 1; i < q.terms.size(); i++) {
            Compiled r = term(q.terms[i]);
            const QueryTerm& t = q.terms[i];
            int line = t.block ? t.block->line : t.paren->line, col = t.block ? t.block->col : t.paren->col;
            std::string name = q.ops[i - 1] == "union_all" ? "UNION ALL" : upper(q.ops[i - 1]);
            if (acc.cols.size() != r.cols.size())
                sqlFail("schema", "the inputs of " + name + " have " + std::to_string(acc.cols.size()) + " and " + std::to_string(r.cols.size()) + " columns", line, col);
            for (size_t k = 0; k < acc.cols.size(); k++) {
                if (!comparable(acc.cols[k].type, r.cols[k].type))
                    sqlFail("schema", "column " + std::to_string(k + 1) + " of " + name + " is " + typeName(acc.cols[k]) + " on the left and " + typeName(r.cols[k]) + " on the right", line, col);
                if (acc.cols[k].type == StrT) acc.cols[k].length = std::max(acc.cols[k].length, r.cols[k].length);
                else if (acc.cols[k].type != r.cols[k].type) acc.cols[k].type = acc.cols[k].type == RealT || r.cols[k].type == RealT ? RealT : BigT;
            }
            acc.node = logicalNode(q.ops[i - 1], {std::move(acc.node), std::move(r.node)});
        }
        if (!q.orderBy.empty()) {
            LogicalNode s = unaryNode("sort", std::move(acc.node));
            for (const OrderItem& o : q.orderBy) {
                if (o.isAgg || !o.column.qualifier.empty()) sqlFail("name", "ORDER BY after a set operation can only name the result's columns", o.line, o.col);
                std::vector<const Col*> hits;
                for (const Col& c : acc.cols) if (c.name == o.column.name) hits.push_back(&c);
                if (hits.empty()) sqlFail("name", "ORDER BY " + o.column.name + " is not a column of the result", o.line, o.col);
                if (hits.size() > 1) sqlFail("ambiguity", "ORDER BY " + o.column.name + " could be " + qualified(*hits[0]) + " or " + qualified(*hits[1]), o.line, o.col);
                s.sortKeys.emplace_back(qualified(*hits[0]), o.desc);
            }
            acc.node = std::move(s);
        }
        if (q.limit >= 0) { acc.node = unaryNode("limit", std::move(acc.node)); acc.node.n = q.limit; }
        return acc;
    }

    // a CREATE TABLE or INSERT is checked here, and later statements of the same script see the new table
    Value insertValue(const SqlOperand& v, const Col& c) {
        if (v.kind == SqlOperand::Int && c.type == IntT) {
            long long value = intLiteral(v);
            return {true, double(value), std::to_string(value)};
        }
        if (v.kind == SqlOperand::Str && c.type == StrT) {
            if (int(v.literal.size()) > c.length) sqlFail("type", quoted(v.literal) + " is longer than " + typeName(c), v.line, v.col);
            return {false, 0, v.literal};
        }
        sqlFail("type", "column " + c.name + " is " + typeName(c) + " but the value " + operandSql(v) + " is " + (v.kind == SqlOperand::Int ? "an INT" : "a string"), v.line, v.col);
    }
    std::vector<Col> createTable(const Statement& s) {
        if (schemaOf(s.table)) sqlFail("name", "table " + s.table + " already exists", s.nameLine, s.nameCol);
        if (std::string problem = tableNameProblem(s.table); !problem.empty()) sqlFail("name", problem, s.nameLine, s.nameCol);
        std::vector<Col> cols;
        for (const ColumnDef& d : s.columns) {
            for (const Col& c : cols) if (c.name == d.name) sqlFail("schema", "column " + d.name + " appears twice in table " + s.table, d.line, d.col);
            cols.push_back({s.table, d.name, d.varchar ? StrT : IntT, d.length});
        }
        created[s.table] = cols;
        return cols;
    }
    Row insertRow(const Statement& s) {
        const std::vector<Col>* cols = schemaOf(s.table);
        if (!cols) sqlFail("name", "unknown table " + s.table, s.line, s.col);
        if (cols->size() != s.values.size())
            sqlFail("schema", "table " + s.table + " has " + std::to_string(cols->size()) + " columns but " + std::to_string(s.values.size()) + " values were given", s.line, s.col);
        Row row;
        for (size_t i = 0; i < cols->size(); i++) row.push_back(insertValue(s.values[i], (*cols)[i]));
        return row;
    }
    Json statementJson(const Statement& s) {
        if (s.kind == Statement::Select) return logicalJson(query(*s.query).node);
        Json j = jsonObject();
        if (s.kind == Statement::Create) {
            Json cols = jsonArray();
            for (const Col& c : createTable(s)) {
                Json col = jsonArray().push(jsonString(c.name)).push(jsonString(c.type == StrT ? "VARCHAR" : "INT"));
                if (c.type == StrT) col.push(jsonNumber(c.length));
                cols.push(col);
            }
            return j.add("statement", jsonString("create_table")).add("table", jsonString(s.table)).add("columns", cols);
        }
        Json values = jsonArray();
        for (const Value& v : insertRow(s)) values.push(v.isNum ? jsonNumber(v.text) : jsonString(v.text));
        return j.add("statement", jsonString("insert")).add("table", jsonString(s.table)).add("values", values);
    }
};
