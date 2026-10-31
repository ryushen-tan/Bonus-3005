// physical plan JSON: checking it against the schema, building the operator tree, and the exec tool
#pragma once
#include "exec.hpp"
#include "storage.hpp"
#include <chrono>

inline std::string kindName(const Json& j) {
    static const char* const NAMES[] = {"null", "a boolean", "a number", "a string", "an array", "an object"};
    return NAMES[j.kind];
}
[[noreturn]] inline void planError(const Json& j, const std::string& path, const std::string& problem) {
    const Json* op = j.kind == Json::Object ? j.find("op") : nullptr;
    std::string node = op && op->kind == Json::String ? "Node '" + op->text + "' (" + path + ")" : "Node at " + path;
    throw Error{node + ": " + problem + "."};
}
inline bool isWholeNumber(const Json& j) { return j.kind == Json::Number && std::all_of(j.text.begin(), j.text.end(), dig); }
inline bool isPair(const Json& j) { return j.kind == Json::Array && j.items.size() == 2 && j.items[0].kind == Json::String && j.items[1].kind == Json::String; }

const std::map<std::string, std::vector<std::string>> PLAN_FIELDS = {
    {"seqscan", {"table"}}, {"filter", {"condition", "child"}}, {"project", {"columns", "child"}}, {"rename", {"to", "child"}},
    {"sort", {"keys", "child"}}, {"distinct", {"mode", "child"}}, {"group_by", {"mode", "keys", "aggregates", "child"}}, {"limit", {"n", "child"}},
    {"nested_loop_join", {"left", "right"}}, {"sort_merge_join", {"condition", "left", "right"}}, {"hash_join", {"condition", "left", "right"}},
    {"union_all", {"left", "right"}}, {"union", {"left", "right"}}, {"intersect", {"left", "right"}}, {"except", {"left", "right"}},
};

// every problem with the shape of the plan is found here, before any table is looked at
inline void validatePlan(const Json& j, const std::string& path) {
    if (j.kind != Json::Object) planError(j, path, "must be an object, not " + kindName(j));
    const Json* op = j.find("op");
    if (!op) planError(j, path, "missing required field 'op'");
    if (op->kind != Json::String) planError(j, path, "field 'op' must be a string, not " + kindName(*op));
    auto rule = PLAN_FIELDS.find(op->text);
    if (rule == PLAN_FIELDS.end()) planError(j, path, "unknown operator '" + op->text + "'");
    for (const std::string& field : rule->second) if (!j.find(field)) planError(j, path, "missing required field '" + field + "'");
    for (const auto& member : j.fields) {
        const std::string& field = member.first;
        const Json& value = member.second;
        auto must = [&](bool ok, const std::string& what) { if (!ok) planError(j, path, "field '" + field + "' must be " + what); };
        if (field == "table" || field == "to" || field == "condition") must(value.kind == Json::String && !value.text.empty(), "a non-empty string");
        else if (field == "child" || field == "left" || field == "right") validatePlan(value, path + "." + field);
        else if (field == "mode") must(value.kind == Json::String && (value.text == "sort" || value.text == "hash"), "\"sort\" or \"hash\"");
        else if (field == "n") must(isWholeNumber(value), "a non-negative integer");
        else if (field == "columns")
            must(value.kind == Json::Array && !value.items.empty() && std::all_of(value.items.begin(), value.items.end(), [](const Json& c) { return c.kind == Json::String || isPair(c); }),
                 "a non-empty array of column names or [column, alias] pairs");
        else if (field == "keys" && op->text == "sort")
            must(value.kind == Json::Array && !value.items.empty() &&
                     std::all_of(value.items.begin(), value.items.end(), [](const Json& k) { return isPair(k) && (k.items[1].text == "asc" || k.items[1].text == "desc"); }),
                 "a non-empty array of [column, \"asc\" or \"desc\"] pairs");
        else if (field == "keys") must(value.kind == Json::Array && std::all_of(value.items.begin(), value.items.end(), [](const Json& k) { return k.kind == Json::String; }), "an array of column names");
        else if (field == "aggregates")
            must(value.kind == Json::Array && std::all_of(value.items.begin(), value.items.end(), [](const Json& a) {
                     bool shape = a.kind == Json::Array && (a.items.size() == 2 || a.items.size() == 3) && std::all_of(a.items.begin(), a.items.end(), [](const Json& x) { return x.kind == Json::String; });
                     if (!shape) return false;
                     const std::string& f = a.items[0].text;
                     return (f == "count" || f == "sum" || f == "avg" || f == "min" || f == "max") && (a.items[1].text != "*" || f == "count");
                 }),
                 "an array of [function, column] or [function, column, name] with function count, sum, avg, min or max, and * only for count");
    }
}

inline Node attrNode(const std::string& text) { return mk(Attr, Token{Word, text, 1, 1}); }
inline std::string aggregateName(const std::string& func, const std::string& column) {
    if (column == "*") return func + "_star";
    std::string name = func + "_" + column;
    std::replace(name.begin(), name.end(), '.', '_');
    return name;
}
// the wider of two union compatible column types, which the output of a set operation takes
inline Col widerColumn(Col a, const Col& b) {
    if (a.type == StrT) a.length = std::max(a.length, b.length);
    else if (a.type != b.type) a.type = a.type == RealT || b.type == RealT ? RealT : BigT;
    return a;
}

// a condition that names a column of each input in every one of its equalities, which is all a hash or
// sort-merge join can evaluate; left and right are column positions within each input
inline void equalityKeys(const Node& c, size_t leftWidth, JoinCondition& join) {
    if (c.kind == And) { equalityKeys(c.kids[0], leftWidth, join); equalityKeys(c.kids[1], leftWidth, join); return; }
    bool ok = c.kind == Eq && c.kids[0].kind == Attr && c.kids[1].kind == Attr && (c.kids[0].idx < leftWidth) != (c.kids[1].idx < leftWidth);
    if (!ok) throw Error{"condition '" + condText(c) + "' is not an equality between a column of each input"};
    size_t l = std::min(c.kids[0].idx, c.kids[1].idx), r = std::max(c.kids[0].idx, c.kids[1].idx);
    join.left.push_back(l);
    join.right.push_back(r - leftWidth);
}

inline bool holdsMemoryBelow(const Operator& op) {
    if (op.holdsMemory()) return true;
    for (const auto& c : op.children) if (holdsMemoryBelow(*c)) return true;
    return false;
}

inline std::unique_ptr<Operator> buildNode(Database& db, const Json& j, const std::string& path) {
    const std::string& op = j.find("op")->text;
    auto text = [&](const char* field) -> const std::string& { return j.find(field)->text; };
    std::vector<std::unique_ptr<Operator>> kids;
    for (const char* field : {"child", "left", "right"}) if (j.find(field)) kids.push_back(buildNode(db, *j.find(field), path + "." + field));
    std::unique_ptr<Operator> node;
    try {
        if (op == "seqscan") {
            Table* t = db.find(text("table"));
            if (!t) throw Error{"unknown table '" + text("table") + "'"};
            node = std::make_unique<SeqScan>(db, *t);
        }
        else if (op == "filter") {
            Node cond = parseCondition(text("condition"));
            bindCond(cond, kids[0]->schema);
            node = std::make_unique<Filter>(db, std::move(cond));
            node->schema = kids[0]->schema;
        }
        else if (op == "project") {
            auto p = std::make_unique<Projection>(db);
            for (const Json& c : j.find("columns")->items) {
                Node a = attrNode(c.kind == Json::String ? c.text : c.items[0].text);
                Col col = kids[0]->schema[resolve(kids[0]->schema, a)];
                if (c.kind == Json::Array) col = {"", c.items[1].text, col.type, col.length};
                p->columns.push_back(a.idx);
                p->schema.push_back(col);
            }
            checkUnique(p->schema, 0, 0, "project");
            node = std::move(p);
        }
        else if (op == "rename") {
            node = std::make_unique<Renaming>(db);
            node->detail = text("to");
            node->schema = kids[0]->schema;
            for (Col& c : node->schema) c.rel = text("to");
            checkUnique(node->schema, 0, 0, "rename");
        }
        else if (op == "limit") node = std::make_unique<Limit>(db, std::stoll(text("n").size() > 18 ? "999999999999999999" : text("n")));
        else if (op == "sort") {
            auto s = std::make_unique<Sort>(db);
            for (const Json& k : j.find("keys")->items) { Node a = attrNode(k.items[0].text); s->keys.push_back({resolve(kids[0]->schema, a), k.items[1].text == "desc"}); }
            s->schema = kids[0]->schema;
            node = std::move(s);
        }
        else if (op == "distinct") {
            if (text("mode") == "hash") node = std::make_unique<HashDistinct>(db); else node = std::make_unique<SortDistinct>(db);
            node->schema = kids[0]->schema;
        }
        else if (op == "group_by") {
            std::vector<size_t> keys;
            std::vector<Aggregate> aggs;
            std::vector<Col> out;
            const std::vector<Col>& in = kids[0]->schema;
            for (const Json& k : j.find("keys")->items) { Node a = attrNode(k.text); keys.push_back(resolve(in, a)); out.push_back(in[keys.back()]); }
            for (const Json& a : j.find("aggregates")->items) {
                const std::string& func = a.items[0].text;
                Aggregate agg{func, -1, a.items.size() == 3 ? a.items[2].text : aggregateName(func, a.items[1].text)};
                Col col{"", agg.name, IntT};
                if (a.items[1].text != "*") {
                    Node c = attrNode(a.items[1].text);
                    const Col& source = in[resolve(in, c)];
                    agg.column = (long long)c.idx;
                    if ((func == "sum" || func == "avg") && source.type != IntT && source.type != BigT)
                        throw Error{"type error: " + func + " needs an INT column, " + a.items[1].text + " is " + typeName(source)};
                    if (func == "sum") col.type = BigT;
                    else if (func == "avg") col.type = RealT;
                    else if (func != "count") { col.type = source.type; col.length = source.length; }
                }
                aggs.push_back(agg);
                out.push_back(col);
            }
            checkUnique(out, 0, 0, "group_by");
            if (text("mode") == "hash") { auto g = std::make_unique<HashGroupBy>(db); g->keyColumns = keys; g->aggs = aggs; node = std::move(g); }
            else { auto g = std::make_unique<SortGroupBy>(db); g->keyColumns = keys; g->aggs = aggs; node = std::move(g); }
            node->schema = out;
        }
        else if (op == "nested_loop_join" || op == "hash_join" || op == "sort_merge_join") {
            std::vector<Col> both = kids[0]->schema;
            both.insert(both.end(), kids[1]->schema.begin(), kids[1]->schema.end());
            checkUnique(both, 0, 0, op);
            JoinCondition join;
            if (j.find("condition")) {
                join.cond = std::make_unique<Node>(parseCondition(text("condition")));
                bindCond(*join.cond, both);
                if (op != "nested_loop_join") equalityKeys(*join.cond, kids[0]->schema.size(), join);
            }
            if (op == "hash_join") { auto h = std::make_unique<HashJoin>(db); h->join = std::move(join); node = std::move(h); }
            else if (op == "sort_merge_join") { auto s = std::make_unique<SortMergeJoin>(db); s->join = std::move(join); node = std::move(s); }
            else { auto n = std::make_unique<NestedLoopJoin>(db); n->join = std::move(join); node = std::move(n); }
            node->schema = both;
        }
        else {
            const std::vector<Col>& l = kids[0]->schema;
            const std::vector<Col>& r = kids[1]->schema;
            if (l.size() != r.size()) throw Error{"inputs have " + std::to_string(l.size()) + " and " + std::to_string(r.size()) + " columns"};
            std::vector<Col> out;
            for (size_t i = 0; i < l.size(); i++) {
                if (!comparable(l[i].type, r[i].type)) throw Error{"column " + std::to_string(i + 1) + " is " + typeName(l[i]) + " on the left and " + typeName(r[i]) + " on the right"};
                out.push_back(widerColumn(l[i], r[i]));
            }
            if (op == "union_all") node = std::make_unique<UnionAll>(db);
            else if (op == "union") node = std::make_unique<SortUnion>(db);
            else node = std::make_unique<SetDifference>(db, op == "intersect");
            node->schema = out;
        }
    } catch (const Error& e) {
        planError(j, path, e.msg + (j.find("condition") ? " in condition '" + text("condition") + "'" : ""));
    }
    node->children = std::move(kids);
    node->materialize.assign(node->children.size(), false);
    if (node->holdsMemory())
        for (size_t i = 0; i < node->children.size(); i++) node->materialize[i] = holdsMemoryBelow(*node->children[i]);
    return node;
}
inline std::unique_ptr<Operator> buildPlan(Database& db, const Json& plan) {
    validatePlan(plan, "root");
    return buildNode(db, plan, "root");
}

struct Result { std::vector<Col> schema; std::vector<Row> rows; double seconds = 0; int peakFrames = 0; };
inline Result execute(Database& db, Operator& root) {
    Result r{root.schema, {}, 0, 0};
    db.pool.clear();
    auto t0 = std::chrono::steady_clock::now();
    root.open();
    Row row;
    while (root.next(row)) r.rows.push_back(row);
    root.close();
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.peakFrames = db.pool.peak;
    return r;
}
inline Counters totals(const Operator& op) {
    Counters c = op.counters;
    for (const auto& kid : op.children) {
        Counters k = totals(*kid);
        c.comparisons += k.comparisons; c.io.reads += k.io.reads; c.io.writes += k.io.writes;
    }
    return c;
}
inline std::string label(const Operator& op) { return op.detail.empty() ? op.op : op.op + " " + op.detail; }
inline void traceLines(const Operator& op, int depth, std::vector<std::pair<std::string, const Operator*>>& lines) {
    lines.emplace_back(std::string(size_t(2 * depth), ' ') + label(op), &op);
    for (const auto& kid : op.children) traceLines(*kid, depth + 1, lines);
}
inline void printTrace(const Operator& root, std::ostream& out) {
    std::vector<std::pair<std::string, const Operator*>> lines;
    traceLines(root, 0, lines);
    size_t width = 0;
    for (const auto& l : lines) width = std::max(width, l.first.size());
    auto field = [](const std::string& name, unsigned long long v, size_t w) { std::string s = name + "=" + std::to_string(v); return s + std::string(s.size() < w ? w - s.size() : 1, ' '); };
    for (const auto& [text, op] : lines) {
        out << text << std::string(width + 2 - text.size(), ' ') << field("tuples_out", op->counters.tuplesOut, 20) << field("comparisons", op->counters.comparisons, 24)
            << field("reads", op->counters.io.reads, 14) << field("writes", op->counters.io.writes, 14) << op->notes << '\n';
    }
}
inline Json resultJson(const Result& r, const Operator& root) {
    Json rows = jsonArray();
    for (const Row& row : r.rows) rows.push(rowJson(row));
    Counters t = totals(root);
    Json totalsJson = jsonObject().add("tuples_out", jsonNumber((long long)root.counters.tuplesOut)).add("comparisons", jsonNumber((long long)t.comparisons))
                                  .add("page_reads", jsonNumber((long long)t.io.reads)).add("page_writes", jsonNumber((long long)t.io.writes));
    return jsonObject().add("schema", schemaJson(r.schema, true)).add("rows", rows).add("totals", totalsJson).add("peak_frames", jsonNumber(r.peakFrames));
}

inline int execTool(const std::vector<std::string>& args, std::ostream& out) {
    const char* usage = "usage: exec run   --db DIR --plan plan.json [--frames B] [--policy lru|clock] [--json]\n"
                        "       exec trace --db DIR --plan plan.json [--frames B] [--policy lru|clock]\n";
    try {
        Args a = parseArgs(args, {"json"});
        if (a.command != "run" && a.command != "trace") { out << usage; return 2; }
        a.allow({"db", "plan", "frames", "policy", "json"});
        int frames = a.number("frames", 32);
        if (frames < 1) throw Error{"error: --frames must be at least 1"};
        Json plan = parseJson(readFile(a.need("plan")));
        if (!std::filesystem::exists(a.need("db") + "/catalog.db")) throw Error{"error: " + a.need("db") + " is not a database, it has no catalog.db"};
        Database db(a.need("db"), frames, policyNamed(a.get("policy", "lru")));
        std::unique_ptr<Operator> root = buildPlan(db, plan);
        Result r = execute(db, *root);
        if (a.command == "trace") {
            printTrace(*root, out);
            Counters t = totals(*root);
            out << "total: tuples_out=" << root->counters.tuplesOut << " comparisons=" << t.comparisons << " reads=" << t.io.reads << " writes=" << t.io.writes << " peak_frames=" << r.peakFrames << '\n';
        }
        else if (a.has("json")) out << dumpJson(resultJson(r, *root)) << '\n';
        else {
            for (size_t i = 0; i < r.schema.size(); i++) out << (i ? ", " : "") << qualified(r.schema[i]);
            out << '\n';
            for (const Row& row : r.rows) out << rowText(row) << '\n';
            Counters t = totals(*root);
            out << "(" << r.rows.size() << " rows) comparisons=" << t.comparisons << " reads=" << t.io.reads << " writes=" << t.io.writes << " peak_frames=" << r.peakFrames << '\n';
        }
        return 0;
    } catch (const Error& e) {
        return report(e, out);
    }
}
