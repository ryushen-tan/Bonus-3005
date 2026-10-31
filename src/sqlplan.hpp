// Module 3, the meaning half: scopes and name resolution, type and aggregate rules, and translation into
// a logical plan built in the canonical order of section 5 of the module document
#pragma once
#include "algebra.hpp"
#include "logical.hpp"
#include "sqlparse.hpp"
#include <functional>
#include <map>

// one entry per table reference in FROM, named by its alias or its table, with that name on every column
struct ScopeEntry { std::string name; std::vector<Col> cols; };
struct Compiled { LogicalNode node; std::vector<Col> cols; };

struct AggregateUse { std::string func, column, name; Col out; };
// what an aggregating block groups by and computes, shared by its select list, HAVING and ORDER BY
struct Grouping { std::vector<Col> keys; std::vector<AggregateUse> aggs; };

[[noreturn]] inline void sqlFail(const std::string& category, const std::string& message, int line, int col) { throw SqlError{category, message, line, col}; }
inline bool sameColumn(const Col& a, const Col& b) { return a.rel == b.rel && a.name == b.name; }
inline LogicalNode unaryNode(const std::string& op, LogicalNode child) { return logicalNode(op, {std::move(child)}); }

struct SqlCompiler {
    std::function<const std::vector<Col>*(const std::string&)> tables;
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
                for (const Col& col : e.cols) if (col.name == c.name) return col;
                sqlFail("name", "column " + c.name + " does not exist in " + c.qualifier, c.line, c.col);
            }
            sqlFail("name", "unknown table or alias " + c.qualifier + " in " + refText(c), c.line, c.col);
        }
        std::vector<Col> hits;
        for (const ScopeEntry& e : scope) for (const Col& col : e.cols) if (col.name == c.name) hits.push_back(col);
        if (hits.empty()) sqlFail("name", "unknown column " + c.name, c.line, c.col);
        if (hits.size() > 1) {
            std::string candidates;
            for (size_t i = 0; i < hits.size(); i++) candidates += (i == 0 ? "" : i + 1 == hits.size() ? " or " : ", ") + qualified(hits[i]);
            sqlFail("ambiguity", "column " + c.name + " is ambiguous, it could be " + candidates, c.line, c.col);
        }
        return hits[0];
    }

    // the aggregate's output column, reusing an identical one already computed unless it is named
    size_t useAggregate(Grouping& g, const AggCall& a, const std::vector<ScopeEntry>& scope, const std::string& alias) {
        Col out{"", "", IntT};
        std::string column = "*";
        if (!a.star) {
            Col source = resolveColumn(a.column, scope);
            column = qualified(source);
            if ((a.func == "sum" || a.func == "avg") && source.type != IntT)
                sqlFail("type", upper(a.func) + " needs an INT column, " + column + " is " + typeName(source), a.line, a.col);
            if (a.func == "sum") out.type = BigT;
            else if (a.func == "avg") out.type = RealT;
            else if (a.func != "count") { out.type = source.type; out.length = source.length; }
        }
        if (alias.empty())
            for (size_t i = 0; i < g.aggs.size(); i++) if (g.aggs[i].func == a.func && g.aggs[i].column == column) return i;
        out.name = alias.empty() ? aggregateName(a.func, column) : alias;
        for (const AggregateUse& u : g.aggs) if (u.name == out.name) sqlFail("schema", "two aggregates are both named " + out.name, a.line, a.col);
        g.aggs.push_back({a.func, column, out.name, out});
        return g.aggs.size() - 1;
    }
    bool isKey(const Grouping& g, const Col& c) { return std::any_of(g.keys.begin(), g.keys.end(), [&](const Col& k) { return sameColumn(k, c); }); }

    // a condition as a Bonus 1 condition tree, which the executor parses back from its text; in HAVING the
    // aggregates become the names of the group_by's output columns
    std::pair<Node, Col> operandNode(const SqlOperand& o, const std::vector<ScopeEntry>& scope, Grouping* having, const std::string& clause) {
        switch (o.kind) {
            case SqlOperand::Int:
                if (o.literal.size() > 11 || std::stoll(o.literal) < INT32_MIN || std::stoll(o.literal) > INT32_MAX) sqlFail("type", o.literal + " does not fit in an INT", o.line, o.col);
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
            Node l = conditionNode(c.kids[0], scope, having, clause), r = conditionNode(c.kids[1], scope, having, clause);
            return mk(c.kind == Condition::Or ? Or : And, t, std::move(l), std::move(r));
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
            Compiled c = query(*r.sub);
            for (const Col& col : c.cols) {
                for (const Col& seen : entry.cols)
                    if (seen.name == col.name) sqlFail("schema", "subquery " + name + " has two columns named " + col.name + ", give one of them an alias", r.line, r.col);
                entry.cols.push_back({name, col.name, col.type, col.length});
            }
            node = unaryNode("rename", std::move(c.node));
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
        // 1 FROM: each JOIN chain left to right, the chains combined by cross products
        std::vector<ScopeEntry> scope;
        LogicalNode node;
        for (size_t ci = 0; ci < b.from.size(); ci++) {
            const JoinChain& chain = b.from[ci];
            LogicalNode joined = tableNode(chain.refs[0], scope);
            for (size_t k = 1; k < chain.refs.size(); k++) {
                LogicalNode right = tableNode(chain.refs[k], scope);
                LogicalNode j = logicalNode("join", {std::move(joined), std::move(right)});
                j.condition = condText(conditionNode(chain.on[k - 1], scope, nullptr, "JOIN ... ON"));
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
            for (const ColumnRef& ref : b.groupBy) g.keys.push_back(resolveColumn(ref, scope));
            for (size_t i = 0; i < b.items.size(); i++) if (b.items[i].isAgg) itemAggregate[i] = useAggregate(g, b.items[i].agg, scope, b.items[i].alias);
            std::string having;
            if (b.having) having = condText(conditionNode(*b.having, scope, &g, "HAVING"));
            for (size_t i = 0; i < orderBy.size(); i++) if (orderBy[i].isAgg) orderAggregate[i] = useAggregate(g, orderBy[i].agg, scope, "");
            node = unaryNode("group_by", std::move(node));
            for (const Col& k : g.keys) node.keys.push_back(qualified(k));
            for (const AggregateUse& u : g.aggs) node.aggregates.push_back({u.func, u.column, u.name});
            if (b.having) { node = unaryNode("select", std::move(node)); node.condition = having; }
        }
        // 5 SELECT: the projection, then distinct if it was asked for and can change anything
        std::vector<std::pair<std::string, std::string>> columns;
        std::vector<Col> out;
        auto addColumn = [&](const Col& c, const std::string& alias, int line, int col) {
            if (aggregating && !isKey(g, c)) sqlFail("aggregate", "column " + qualified(c) + " must appear in GROUP BY or be used in an aggregate function", line, col);
            Col shown = alias.empty() ? c : Col{"", alias, c.type, c.length};
            for (const Col& o : out) if (sameColumn(o, shown)) sqlFail("schema", "the select list has " + qualified(shown) + " twice", line, col);
            columns.emplace_back(qualified(c), alias);
            out.push_back(shown);
        };
        if (b.star) for (const ScopeEntry& e : scope) for (const Col& c : e.cols) addColumn(c, "", b.line, b.col);
        for (size_t i = 0; i < b.items.size(); i++) {
            const SelectItem& item = b.items[i];
            if (!item.isAgg) { addColumn(resolveColumn(item.column, scope), item.alias, item.line, item.col); continue; }
            const AggregateUse& u = g.aggs[itemAggregate[i]];
            for (const Col& o : out) if (sameColumn(o, u.out)) sqlFail("schema", "the select list has " + u.name + " twice", item.line, item.col);
            columns.emplace_back(u.name, "");
            out.push_back(u.out);
        }
        size_t visible = columns.size();
        bool allKeysShown = std::all_of(g.keys.begin(), g.keys.end(), [&](const Col& k) { return std::any_of(out.begin(), out.end(), [&](const Col& o) { return sameColumn(o, k); }); });
        bool distinct = b.distinct && !(aggregating && allKeysShown);
        // 6 ORDER BY: output names first, then the FROM scope; a column the select list dropped is carried
        // through the projection and dropped after the sort
        std::vector<std::pair<std::string, bool>> keys;
        for (size_t i = 0; i < orderBy.size(); i++) {
            const OrderItem& o = orderBy[i];
            std::string key;
            if (o.isAgg) {
                const AggregateUse& u = g.aggs[orderAggregate[i]];
                key = u.name;
                bool shown = std::any_of(out.begin(), out.end(), [&](const Col& c) { return sameColumn(c, u.out); });
                if (!shown && b.distinct) sqlFail("name", "with SELECT DISTINCT, ORDER BY " + aggText(o.agg) + " must be in the select list", o.agg.line, o.agg.col);
                if (!shown && std::none_of(columns.begin(), columns.end(), [&](const auto& c) { return c.first == key && c.second.empty(); })) columns.emplace_back(key, "");
            } else {
                for (size_t k = 0; k < b.items.size() && key.empty() && o.column.qualifier.empty(); k++)
                    if (!b.items[k].alias.empty() && b.items[k].alias == o.column.name) key = o.column.name;
                if (key.empty()) {
                    Col c = resolveColumn(o.column, scope);
                    if (aggregating && !isKey(g, c)) sqlFail("aggregate", "column " + qualified(c) + " must appear in GROUP BY or be used in an aggregate function", o.column.line, o.column.col);
                    key = qualified(c);
                    bool shown = std::any_of(out.begin(), out.end(), [&](const Col& x) { return sameColumn(x, c); });
                    if (!shown && b.distinct) sqlFail("name", "with SELECT DISTINCT, ORDER BY " + refText(o.column) + " must be in the select list", o.column.line, o.column.col);
                    if (!shown && std::none_of(columns.begin(), columns.end(), [&](const auto& x) { return x.first == key && x.second.empty(); })) columns.emplace_back(key, "");
                }
            }
            keys.emplace_back(key, o.desc);
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
    // 8 the set operators between whole blocks, left associative; then ORDER BY and LIMIT of the whole
    Compiled query(const Query& q) {
        if (q.terms.size() == 1 && q.terms[0].block) return block(*q.terms[0].block, q.orderBy, q.limit);
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
                if (o.isAgg) sqlFail("name", "ORDER BY after a set operation can only name the result's columns", o.agg.line, o.agg.col);
                std::vector<const Col*> hits;
                for (const Col& c : acc.cols) if (c.name == o.column.name && (o.column.qualifier.empty() || c.rel == o.column.qualifier)) hits.push_back(&c);
                if (hits.empty()) sqlFail("name", "ORDER BY " + refText(o.column) + " is not a column of the result", o.column.line, o.column.col);
                if (hits.size() > 1) sqlFail("ambiguity", "ORDER BY " + refText(o.column) + " could be " + qualified(*hits[0]) + " or " + qualified(*hits[1]), o.column.line, o.column.col);
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
            if (v.literal.size() > 11 || std::stoll(v.literal) < INT32_MIN || std::stoll(v.literal) > INT32_MAX) sqlFail("type", v.literal + " does not fit in an INT", v.line, v.col);
            return {true, double(std::stoll(v.literal)), std::to_string(std::stoll(v.literal))};
        }
        if (v.kind == SqlOperand::Str && c.type == StrT) {
            if (int(v.literal.size()) > c.length) sqlFail("type", quoted(v.literal) + " is longer than " + typeName(c), v.line, v.col);
            return {false, 0, v.literal};
        }
        sqlFail("type", "column " + c.name + " is " + typeName(c) + " but the value " + operandSql(v) + " is " + (v.kind == SqlOperand::Int ? "an INT" : "a string"), v.line, v.col);
    }
    std::vector<Col> createTable(const Statement& s) {
        if (schemaOf(s.table)) sqlFail("name", "table " + s.table + " already exists", s.line, s.col);
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
