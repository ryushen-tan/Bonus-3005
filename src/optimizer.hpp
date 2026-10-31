// Module 4: statistics, selectivity, the rewrite rules, the cost model, and join order by dynamic programming
#pragma once
#include "plan.hpp"
#include "sqlplan.hpp"
#include <cmath>

// ---- ANALYZE: one scan writes each column to its own run, and each run goes through the external sort ----

inline void analyzeTable(Database& db, Table& t, int buckets) {
    if (buckets < 1) throw Error{"error: the number of buckets must be at least 1"};
    size_t columns = t.cols.size();
    if (db.pool.freeFrames() < int(columns) + 3)
        throw Error{"error: ANALYZE of " + t.name + " needs at least " + std::to_string(columns + 3) + " frames: one per column, one for the scan, and three for a sort"};
    SpillFile spill(db);
    std::vector<std::vector<Col>> single;
    for (const Col& c : t.cols) single.push_back({c});
    std::vector<Run> runs;
    long long tuples = 0;
    double bytes = 0;
    std::vector<double> columnBytes(columns, 0);
    {
        std::vector<std::unique_ptr<RunWriter>> writers;
        for (size_t i = 0; i < columns; i++) writers.push_back(std::make_unique<RunWriter>(db, spill.file, single[i]));
        HeapScan scan(db.pool, t.file);
        Rid rid;
        std::string_view record;
        while (scan.next(rid, record)) {
            tuples++;
            bytes += double(record.size());
            Row row = decode(t.cols, record);
            for (size_t i = 0; i < columns; i++) { columnBytes[i] += double(encodedSize(single[i], {row[i]})); writers[i]->add({row[i]}); }
        }
        for (auto& w : writers) runs.push_back(w->finish());
    }
    TableStats stats;
    stats.analyzed = true;
    stats.tuples = tuples;
    stats.pages = t.dataPages;
    stats.width = tuples ? bytes / double(tuples) : 0;
    unsigned long long comparisons = 0;
    long long k = std::min<long long>(buckets, tuples);
    for (size_t i = 0; i < columns; i++) {
        ColumnStats cs;
        cs.width = tuples ? columnBytes[i] / double(tuples) : 0;
        ExternalSort sorter(db, single[i], {{0, false}}, comparisons);
        {
            RunSource in(db, std::move(runs[i]), single[i]);
            sorter.generate(in);
        }
        sorter.merge(false);
        sorter.startFinal();
        Row row;
        long long index = 0, bucket = -1;
        while (sorter.next(row)) {
            const Value& v = row[0];
            // equi-depth: value number index goes into bucket floor(index * k / n), so the buckets differ by at most one tuple
            long long b = index * k / tuples;
            if (b != bucket) { cs.histogram.push_back({v, v, 0, 0}); bucket = b; }
            Bucket& cur = cs.histogram.back();
            if (cur.count == 0 || compare(v, cur.hi) != 0) cur.ndv++;
            cur.count++;
            cur.hi = v;
            if (index == 0 || compare(v, cs.max) != 0) cs.ndv++;
            if (index == 0) cs.min = v;
            cs.max = v;
            index++;
        }
        sorter.close();
        stats.columns.push_back(cs);
    }
    t.stats = stats;
    db.saveCatalog();
}

// ---- estimates ----

// what the optimizer knows about one output column
struct ColumnInfo { Col col; const ColumnStats* stats = nullptr; double baseTuples = 0, ndv = 1, width = 4; };
inline double defaultWidth(const Col& c) { return c.type == StrT ? 2 + c.length / 2.0 : c.type == IntT ? 4 : 8; }
inline std::vector<Col> colsOf(const std::vector<ColumnInfo>& infos) { std::vector<Col> cols; for (const ColumnInfo& i : infos) cols.push_back(i.col); return cols; }

// the fraction of a column's values below v, from its histogram or else its range
inline double fractionBelow(const ColumnInfo& c, const Value& v) {
    const ColumnStats* s = c.stats;
    if (!s || s->ndv == 0) return 1.0 / 3;
    if (!s->histogram.empty()) {
        double below = 0;
        for (const Bucket& b : s->histogram) {
            if (compare(b.hi, v) < 0) below += double(b.count);
            else if (compare(b.lo, v) < 0) below += double(b.count) * (v.isNum && b.hi.num > b.lo.num ? (v.num - b.lo.num) / (b.hi.num - b.lo.num) : 0.5);
        }
        return below / c.baseTuples;
    }
    if (!v.isNum) return 1.0 / 3;
    if (s->max.num == s->min.num) return v.num > s->min.num ? 1 : 0;
    return std::clamp((v.num - s->min.num) / (s->max.num - s->min.num), 0.0, 1.0);
}
inline double fractionEqual(const ColumnInfo& c, const Value& v) {
    const ColumnStats* s = c.stats;
    if (!s) return 0.1;
    if (s->ndv == 0 || compare(v, s->min) < 0 || compare(v, s->max) > 0) return 0;
    if (s->histogram.empty()) return 1 / double(s->ndv);
    // the spec's count / (n * ndv) for the bucket holding v, summed when v is in more than one bucket
    double rows = 0;
    for (const Bucket& b : s->histogram) if (compare(b.lo, v) <= 0 && compare(v, b.hi) <= 0) rows += double(b.count) / double(std::max(1LL, b.ndv));
    return rows / c.baseTuples;
}
inline double selectivity(const Node& c, const std::vector<ColumnInfo>& cols) {
    switch (c.kind) {
        case And: return selectivity(c.kids[0], cols) * selectivity(c.kids[1], cols);
        case Or: { double p = selectivity(c.kids[0], cols), q = selectivity(c.kids[1], cols); return p + q - p * q; }
        case Not: return 1 - selectivity(c.kids[0], cols);
        default: break;
    }
    const Node &l = c.kids[0], &r = c.kids[1];
    if (l.kind == Attr && r.kind == Attr) {
        if (c.kind != Eq) return 0.1;
        return 1 / std::max(cols[l.idx].ndv, cols[r.idx].ndv);
    }
    if (l.kind != Attr && r.kind != Attr) return eval(c, {}) ? 1 : 0;
    // column op value, turning value op column around
    static const Kind FLIP[] = {Eq, Ne, Gt, Ge, Lt, Le};
    Kind op = l.kind == Attr ? c.kind : FLIP[c.kind - Eq];
    const ColumnInfo& col = cols[l.kind == Attr ? l.idx : r.idx];
    const Value& v = l.kind == Attr ? r.val : l.val;
    double eq = fractionEqual(col, v), below = fractionBelow(col, v);
    switch (op) {
        case Eq: return eq;
        case Ne: return 1 - eq;
        case Lt: case Le: return below;
        default: return 1 - below;
    }
}

// ---- the optimizer ----

// below an operator that holds memory a sort has B - 1 frames, and it needs 3 to merge
constexpr int MIN_PLAN_FRAMES = 4;

struct Candidate { uint32_t outer; int inner; std::string algorithm; double cost; std::vector<int> preds; };
struct Optimizer {
    Database& db;
    int frames;
    std::set<int> disabled;
    std::vector<std::string> dpTable;

    bool on(int rule) const { return !disabled.count(rule); }

    // ---- output columns, without estimates ----
    std::vector<Col> outputCols(const LogicalNode& n) {
        if (n.op == "scan") {
            Table* t = db.find(n.table);
            if (!t) throw Error{"error: unknown table '" + n.table + "'"};
            return t->cols;
        }
        if (n.op == "rename") { std::vector<Col> cols = outputCols(n.kids[0]); for (Col& c : cols) c.rel = n.to; return cols; }
        if (n.op == "project") {
            std::vector<Col> in = outputCols(n.kids[0]), out;
            for (const auto& [src, alias] : n.columns) {
                Node a = attrNode(src);
                Col c = in[resolve(in, a)];
                out.push_back(alias.empty() ? c : Col{"", alias, c.type, c.length});
            }
            return out;
        }
        if (n.op == "join" || n.op == "cross") {
            std::vector<Col> cols = outputCols(n.kids[0]), right = outputCols(n.kids[1]);
            cols.insert(cols.end(), right.begin(), right.end());
            return cols;
        }
        if (n.op == "group_by") {
            std::vector<Col> in = outputCols(n.kids[0]), out;
            for (const std::string& k : n.keys) { Node a = attrNode(k); out.push_back(in[resolve(in, a)]); }
            for (const auto& a : n.aggregates) {
                Col c{"", a[2], IntT};
                if (a[0] == "sum") { Node x = attrNode(a[1]); c.type = in[resolve(in, x)].type == RealT ? RealT : BigT; }
                else if (a[0] == "avg") c.type = RealT;
                else if (a[0] != "count") { Node x = attrNode(a[1]); const Col& s = in[resolve(in, x)]; c.type = s.type; c.length = s.length; }
                out.push_back(c);
            }
            return out;
        }
        return outputCols(n.kids[0]);
    }
    static std::vector<Node> conjuncts(const Node& c) {
        if (c.kind != And) return {c};
        std::vector<Node> out = conjuncts(c.kids[0]), right = conjuncts(c.kids[1]);
        out.insert(out.end(), right.begin(), right.end());
        return out;
    }
    static void attrs(const Node& c, std::vector<std::string>& out) {
        if (c.kind == Attr) out.push_back(c.val.text);
        for (const Node& k : c.kids) attrs(k, out);
    }
    static bool resolvesIn(const std::string& name, const std::vector<Col>& cols) {
        Node a = attrNode(name);
        try { resolve(cols, a); return true; } catch (const Error&) { return false; }
    }
    static bool allIn(const Node& c, const std::vector<Col>& cols) {
        std::vector<std::string> names;
        attrs(c, names);
        return std::all_of(names.begin(), names.end(), [&](const std::string& n) { return resolvesIn(n, cols); });
    }
    static std::string conjunction(std::vector<std::string> parts) {
        std::sort(parts.begin(), parts.end());
        Node all = parseCondition(parts[0]);
        for (size_t i = 1; i < parts.size(); i++) all = mk(And, Token{Word, "and", 1, 1}, std::move(all), parseCondition(parts[i]));
        return condText(all);
    }
    // a condition with every column renamed by f
    static std::string renamed(const std::string& cond, const std::function<std::string(const std::string&)>& f) {
        Node c = parseCondition(cond);
        std::function<void(Node&)> walk = [&](Node& n) { if (n.kind == Attr) n.val.text = f(n.val.text); for (Node& k : n.kids) walk(k); };
        walk(c);
        return condText(c);
    }

    // ---- rules 1 to 5: split each select into its conjuncts and move each one as far down as it may go ----
    LogicalNode wrapSelect(LogicalNode n, const std::string& cond) { LogicalNode s = unaryNode("select", std::move(n)); s.condition = cond; return s; }
    LogicalNode place(LogicalNode n, const std::string& cond) {
        Node c = parseCondition(cond);
        if (n.op == "select") { n.kids[0] = place(std::move(n.kids[0]), cond); return n; }
        if ((n.op == "join" || n.op == "cross") && on(2)) {
            for (size_t side = 0; side < 2; side++)
                if (allIn(c, outputCols(n.kids[side]))) { n.kids[side] = place(std::move(n.kids[side]), cond); return n; }
        }
        if ((n.op == "join" || n.op == "cross") && on(3) && c.kind == Eq && c.kids[0].kind == Attr && c.kids[1].kind == Attr) {
            std::vector<Col> l = outputCols(n.kids[0]), r = outputCols(n.kids[1]);
            bool across = (resolvesIn(c.kids[0].val.text, l) && resolvesIn(c.kids[1].val.text, r)) || (resolvesIn(c.kids[0].val.text, r) && resolvesIn(c.kids[1].val.text, l));
            if (across) {
                std::vector<std::string> parts = {cond};
                if (n.op == "join") for (const Node& k : conjuncts(parseCondition(n.condition))) parts.push_back(condText(k));
                n.condition = conjunction(parts);
                n.op = "join";
                return n;
            }
        }
        // a rename straight over a scan is part of the access path, and moving a select under it gains nothing
        if (n.op == "rename" && on(4) && n.kids[0].op != "scan") {
            std::vector<Col> in = outputCols(n.kids[0]);
            std::string to = n.to;
            n.kids[0] = place(std::move(n.kids[0]), renamed(cond, [&](const std::string& name) {
                std::string column = name.substr(name.find('.') + 1);
                for (const Col& col : in) if (col.name == column) return qualified(col);
                return name;
            }));
            return n;
        }
        if ((n.op == "union" || n.op == "union_all") && on(4)) {
            std::vector<Col> l = outputCols(n.kids[0]), r = outputCols(n.kids[1]);
            n.kids[1] = place(std::move(n.kids[1]), renamed(cond, [&](const std::string& name) { Node a = attrNode(name); return qualified(r[resolve(l, a)]); }));
            n.kids[0] = place(std::move(n.kids[0]), cond);
            return n;
        }
        // with no grouping columns the group_by returns a row even when the select below it removes them all
        // a name is looked up in the group_by's own output, where an aggregate named like a key is not that key
        if (n.op == "group_by" && on(5) && !n.keys.empty()) {
            std::vector<Col> out = outputCols(n);
            std::vector<std::string> names;
            attrs(c, names);
            bool onlyKeys = std::all_of(names.begin(), names.end(), [&](const std::string& name) {
                Node a = attrNode(name);
                return resolvesIn(name, out) && resolve(out, a) < n.keys.size();
            });
            if (onlyKeys) { n.kids[0] = place(std::move(n.kids[0]), cond); return n; }
        }
        return wrapSelect(std::move(n), cond);
    }
    LogicalNode pushSelects(LogicalNode n) {
        for (LogicalNode& k : n.kids) k = pushSelects(std::move(k));
        if (n.op != "select") return n;
        std::vector<std::string> parts;
        if (on(1)) for (const Node& c : conjuncts(parseCondition(n.condition))) parts.push_back(condText(c));
        else parts.push_back(n.condition);
        LogicalNode out = std::move(n.kids[0]);
        // the last conjunct is placed first, so conjuncts that stay put keep their written order top down
        for (size_t i = parts.size(); i-- > 0;) out = place(std::move(out), parts[i]);
        return out;
    }

    // ---- estimates and costs ----
    static bool accessPath(const LogicalNode& n) {
        if (n.op == "scan") return true;
        return (n.op == "select" || n.op == "project" || n.op == "rename") && accessPath(n.kids[0]);
    }
    // the pages read to get a node's output once: a scan's own pages, or the pages of a result
    static double read(const LogicalNode& n) { return accessPath(n) ? n.total : n.pages; }
    // the frames an operator works in: B, or B - 1 below an operator that holds memory, because that one
    // writes its input to a temporary file first and the input must leave the file's frame free
    static bool holdsMemory(const LogicalNode& n) { return n.op != "scan" && n.op != "select" && n.op != "project" && n.op != "rename" && n.op != "limit" && n.op != "union_all"; }
    int memoryBelow(const LogicalNode& n, int memory) const { return holdsMemory(n) ? frames - 1 : memory; }
    static int passes(double pages, int memory) {
        double runs = std::ceil(pages / memory);
        return runs <= 1 ? 1 : 1 + int(std::ceil(std::log(runs) / std::log(memory - 1.0)));
    }
    static double sortCost(const LogicalNode& in, int memory) { return read(in) + 2 * in.pages * (passes(in.pages, memory) - 1); }
    // hash distinct and group_by as Module 2 runs them, records being the pages of one group record per input
    // tuple: one pass when the groups fit in the memory - 1 frames beside the input page. Otherwise each input
    // whose groups do not fit writes its full table out and reads it back, and splits its records memory - 1
    // ways, each partition's last page part full, to be read back at the next level
    static double hashCost(const LogicalNode& in, double records, double groups, int memory) {
        double work = memory - 1, groupPages = records * groups / in.rows, cost = read(in);
        for (double inputs = 1; groupPages / inputs > work; inputs *= work) {
            double parts = inputs * work;
            cost += 2 * work * inputs + 2 * parts * std::ceil(records / parts);
        }
        return cost;
    }
    static double pagesFor(double rows, const std::vector<ColumnInfo>& cols) {
        double width = 0;
        for (const ColumnInfo& c : cols) width += c.width;
        return std::max(1.0, std::ceil(rows * width / PAGE_SIZE));
    }
    static double rowsOut(double rows) { return std::max(1.0, std::ceil(rows - 1e-9)); }
    static double joinCost(const std::string& algorithm, const LogicalNode& outer, const LogicalNode& inner, int memory) {
        if (algorithm == "nested_loop_join") return read(outer) + std::ceil(outer.pages / (memory - 2)) * read(inner);
        if (algorithm == "hash_join") return read(outer) + read(inner) + 2 * (outer.pages + inner.pages);
        return sortCost(outer, memory) + sortCost(inner, memory) + outer.pages + inner.pages;
    }
    // fills in rows, pages, own cost and total cost of every node, and returns its columns
    std::vector<ColumnInfo> estimate(LogicalNode& n, int memory) {
        std::vector<std::vector<ColumnInfo>> in;
        for (LogicalNode& k : n.kids) in.push_back(estimate(k, memoryBelow(n, memory)));
        std::vector<ColumnInfo> out;
        double rows = n.kids.empty() ? 0 : n.kids[0].rows;
        n.cost = 0;
        if (n.op == "scan") {
            Table& t = db.table(n.table);
            rows = t.stats.analyzed ? double(t.stats.tuples) : double(t.liveTuples);
            for (size_t i = 0; i < t.cols.size(); i++) {
                ColumnInfo c{t.cols[i], nullptr, rows, std::max(1.0, rows), defaultWidth(t.cols[i])};
                if (t.stats.analyzed) { c.stats = &t.stats.columns[i]; c.ndv = std::max<double>(1, double(c.stats->ndv)); c.width = c.stats->width; }
                out.push_back(c);
            }
            n.rows = rowsOut(rows);
            n.pages = std::max<double>(1, t.dataPages);
            n.cost = n.total = n.pages;
            return out;
        }
        if (n.op == "rename") { out = in[0]; for (ColumnInfo& c : out) c.col.rel = n.to; }
        else if (n.op == "select") {
            out = in[0];
            Node c = parseCondition(n.condition);
            std::vector<Col> cols = colsOf(out);
            bindCond(c, cols);
            rows = rows * selectivity(c, out);
        }
        else if (n.op == "project") {
            std::vector<Col> cols = colsOf(in[0]);
            for (const auto& [src, alias] : n.columns) {
                Node a = attrNode(src);
                ColumnInfo c = in[0][resolve(cols, a)];
                if (!alias.empty()) c.col = {"", alias, c.col.type, c.col.length};
                out.push_back(c);
            }
        }
        else if (n.op == "join" || n.op == "cross") {
            out = in[0];
            out.insert(out.end(), in[1].begin(), in[1].end());
            rows = n.kids[0].rows * n.kids[1].rows;
            if (n.op == "join") {
                Node c = parseCondition(n.condition);
                std::vector<Col> cols = colsOf(out);
                bindCond(c, cols);
                rows *= selectivity(c, out);
            }
            n.cost = joinCost(n.algorithm.empty() ? "nested_loop_join" : n.algorithm, n.kids[0], n.kids[1], memory);
        }
        else if (n.op == "group_by") {
            std::vector<Col> cols = colsOf(in[0]);
            double groups = 1;
            for (const std::string& k : n.keys) { Node a = attrNode(k); out.push_back(in[0][resolve(cols, a)]); groups *= out.back().ndv; }
            rows = n.keys.empty() ? 1 : std::min(rows, groups);
            std::vector<Col> outCols = outputCols(n);
            for (size_t i = n.keys.size(); i < outCols.size(); i++) out.push_back({outCols[i], nullptr, 0, std::max(1.0, rows), defaultWidth(outCols[i])});
            double sortWay = n.keys.empty() ? read(n.kids[0]) : sortCost(n.kids[0], memory);
            double recordWidth = 0;
            for (size_t i = 0; i < n.keys.size(); i++) recordWidth += out[i].width;
            for (const auto& a : n.aggregates) {
                recordWidth += a[0] == "sum" || a[0] == "avg" ? 16 : 8;
                if (a[0] == "min" || a[0] == "max") { Node x = attrNode(a[1]); recordWidth += in[0][resolve(cols, x)].width; }
            }
            double hashWay = hashCost(n.kids[0], std::max(1.0, std::ceil(n.kids[0].rows * recordWidth / PAGE_SIZE)), rows, memory);
            if (n.algorithm.empty()) n.algorithm = n.keys.empty() || sortWay < hashWay ? "sort" : "hash";
            n.cost = n.algorithm == "sort" ? sortWay : hashWay;
        }
        else if (n.op == "distinct") {
            out = in[0];
            double groups = 1;
            for (const ColumnInfo& c : out) groups *= c.ndv;
            rows = std::min(rows, groups);
            double sortWay = sortCost(n.kids[0], memory);
            double hashWay = hashCost(n.kids[0], n.kids[0].pages, rows, memory);
            if (n.algorithm.empty()) n.algorithm = sortWay < hashWay ? "sort" : "hash";
            n.cost = n.algorithm == "sort" ? sortWay : hashWay;
        }
        else if (n.op == "sort") { out = in[0]; n.cost = sortCost(n.kids[0], memory); }
        else if (n.op == "limit") { out = in[0]; rows = std::min(rows, double(n.n)); }
        else {
            out = in[0];
            const LogicalNode &l = n.kids[0], &r = n.kids[1];
            // the set operations that remove duplicates can return no more rows than there are distinct combinations
            double either = 1, both = 1, left = 1;
            for (size_t i = 0; i < out.size(); i++) { either *= in[0][i].ndv + in[1][i].ndv; both *= std::min(in[0][i].ndv, in[1][i].ndv); left *= in[0][i].ndv; }
            if (n.op == "union_all") rows = l.rows + r.rows;
            else if (n.op == "union") rows = std::min(l.rows + r.rows, either);
            else if (n.op == "intersect") rows = std::min({l.rows, r.rows, both});
            else rows = std::min(l.rows, left);
            if (n.op == "union") { double p = l.pages + r.pages; n.cost = read(l) + read(r) + 2 * p * (passes(p, memory) - 1); }
            else if (n.op != "union_all") n.cost = sortCost(l, memory) + sortCost(r, memory) + l.pages + r.pages;
        }
        n.rows = n.op == "limit" ? std::max(0.0, std::min(rowsOut(rows), double(n.n))) : rowsOut(rows);
        for (ColumnInfo& c : out) c.ndv = std::max(1.0, std::min(c.ndv, n.rows));
        n.pages = pagesFor(n.rows, out);
        bool readsInput = n.cost > 0 || n.op == "join" || n.op == "cross" || n.op == "sort" || n.op == "group_by" || n.op == "distinct" || n.op == "union" || n.op == "intersect" || n.op == "except";
        n.total = n.cost;
        for (const LogicalNode& k : n.kids) n.total += readsInput && accessPath(k) ? 0 : k.total;
        return out;
    }

    // ---- join order: dynamic programming over left-deep trees ----
    struct Leaf { LogicalNode node; std::vector<ColumnInfo> cols; std::string name; };
    struct Pred { std::string text; Node cond; uint32_t leaves = 0; };
    // the leaves of a block of joins and the predicates over them; a predicate from a select (not from a
    // join's own condition) is marked, because with rule 3 off it may not become a join condition
    void collect(LogicalNode& n, std::vector<Leaf>& leaves, std::vector<std::pair<std::string, bool>>& preds) {
        auto parts = [&](const std::string& cond) { return on(1) ? conjuncts(parseCondition(cond)) : std::vector<Node>{parseCondition(cond)}; };
        if (n.op == "join" || n.op == "cross") {
            if (n.op == "join") for (const Node& c : parts(n.condition)) preds.emplace_back(condText(c), false);
            collect(n.kids[0], leaves, preds);
            collect(n.kids[1], leaves, preds);
            return;
        }
        if (n.op == "select" && (n.kids[0].op == "join" || n.kids[0].op == "cross" || n.kids[0].op == "select")) {
            LogicalNode* below = &n.kids[0];
            while (below->op == "select") below = &below->kids[0];
            if (below->op == "join" || below->op == "cross") {
                for (const Node& c : parts(n.condition)) preds.emplace_back(condText(c), true);
                collect(n.kids[0], leaves, preds);
                return;
            }
        }
        Leaf leaf{reorder(std::move(n), frames - 1), {}, ""};
        leaf.cols = estimate(leaf.node, frames - 1);
        std::set<std::string> rels;
        for (const ColumnInfo& c : leaf.cols) rels.insert(c.col.rel);
        leaf.name = rels.size() == 1 && !rels.begin()->empty() ? *rels.begin() : "input" + std::to_string(leaves.size() + 1);
        leaves.push_back(std::move(leaf));
    }
    std::string subsetName(uint32_t mask, const std::vector<Leaf>& leaves) {
        std::string s = "{";
        for (size_t i = 0; i < leaves.size(); i++) if (mask >> i & 1) s += (s.size() > 1 ? "," : "") + leaves[i].name;
        return s + "}";
    }
    LogicalNode joinBlock(LogicalNode n, int memory) {
        std::vector<Leaf> leaves;
        std::vector<std::pair<std::string, bool>> texts;
        collect(n, leaves, texts);
        std::vector<Pred> preds;
        std::vector<std::string> above;
        for (const auto& [t, fromSelect] : texts) {
            Pred p{t, parseCondition(t), 0};
            std::vector<std::string> names;
            attrs(p.cond, names);
            for (const std::string& name : names)
                for (size_t i = 0; i < leaves.size(); i++) if (resolvesIn(name, colsOf(leaves[i].cols))) p.leaves |= 1u << i;
            if (__builtin_popcount(p.leaves) >= 2 && !(fromSelect && !on(3))) { preds.push_back(p); continue; }
            if (__builtin_popcount(p.leaves) >= 2) { above.push_back(t); continue; }
            int only = p.leaves ? __builtin_ctz(p.leaves) : 0;
            if (on(2)) { leaves[size_t(only)].node = place(std::move(leaves[size_t(only)].node), t); leaves[size_t(only)].cols = estimate(leaves[size_t(only)].node, frames - 1); }
            else above.push_back(t);
        }
        size_t count = leaves.size();
        uint32_t all = (1u << count) - 1;
        std::vector<ColumnInfo> allCols;
        std::vector<size_t> firstCol;
        for (const Leaf& l : leaves) { firstCol.push_back(allCols.size()); allCols.insert(allCols.end(), l.cols.begin(), l.cols.end()); }
        for (Pred& p : preds) bindCond(p.cond, colsOf(allCols));
        std::map<uint32_t, LogicalNode> best;
        for (size_t i = 0; i < count; i++) {
            best[1u << i] = leaves[i].node;
            dpTable.push_back(subsetName(1u << i, leaves) + "  rows=" + std::to_string((long long)leaves[i].node.rows) + " pages=" + std::to_string((long long)leaves[i].node.pages) +
                              "  * " + (accessPath(leaves[i].node) ? "access path" : "subquery") + "  cost=" + std::to_string((long long)leaves[i].node.total));
        }
        // a predicate joins leaf r to the rest of a subset when its leaves are in the subset and include r and
        // another one, so a predicate over k leaves is applied at the first join that has all k
        auto joins = [&](const Pred& p, uint32_t mask, size_t r) { return (p.leaves & mask) == p.leaves && (p.leaves >> r & 1) && (p.leaves & mask & ~(1u << r)); };
        // the subsets a left-deep plan can build without a cross product. When the whole query is one, the others
        // are skipped. Otherwise (two parts with no predicate between them, or a link only through a predicate
        // over three or more leaves) every subset is built, with a cross product where no predicate joins
        std::vector<bool> withoutCross(size_t(all) + 1, false);
        for (uint32_t mask = 1; mask <= all; mask++)
            for (size_t r = 0; r < count; r++) {
                uint32_t rest = mask & ~(1u << r);
                if ((mask >> r & 1) && (!rest || (withoutCross[rest] && std::any_of(preds.begin(), preds.end(), [&](const Pred& p) { return joins(p, mask, r); })))) withoutCross[mask] = true;
            }
        bool connectedQuery = withoutCross[all];
        for (size_t size = 2; size <= count; size++) {
            int joinMemory = size == count ? memory : frames - 1;
            for (uint32_t mask = 1; mask <= all; mask++) {
                if (size_t(__builtin_popcount(mask)) != size) continue;
                if (connectedQuery && !withoutCross[mask]) { dpTable.push_back(subsetName(mask, leaves) + "  no join predicate connects it, skipped"); continue; }
                double rows = 1;
                for (size_t i = 0; i < count; i++) if (mask >> i & 1) rows *= leaves[i].node.rows;
                for (const Pred& p : preds) if ((p.leaves & mask) == p.leaves) rows *= selectivity(p.cond, allCols);
                std::vector<Candidate> candidates;
                bool connected = false;
                // the last relation first, so that when costs tie the written order wins
                for (size_t r = count; r-- > 0;) {
                    uint32_t rest = mask & ~(1u << r);
                    if (!(mask >> r & 1) || !best.count(rest)) continue;
                    std::vector<int> joining;
                    for (size_t p = 0; p < preds.size(); p++) if (joins(preds[p], mask, r)) joining.push_back(int(p));
                    connected = connected || !joining.empty();
                    bool equi = !joining.empty() && std::all_of(joining.begin(), joining.end(), [&](int p) {
                        const Node& c = preds[size_t(p)].cond;
                        if (c.kind != Eq || c.kids[0].kind != Attr || c.kids[1].kind != Attr) return false;
                        auto inR = [&](size_t idx) { return idx >= firstCol[r] && idx < firstCol[r] + leaves[r].cols.size(); };
                        return inR(c.kids[0].idx) != inR(c.kids[1].idx);
                    });
                    for (const char* alg : {"nested_loop_join", "hash_join", "sort_merge_join"}) {
                        if (std::string(alg) != "nested_loop_join" && !equi) continue;
                        const LogicalNode &outer = best[rest], &inner = leaves[r].node;
                        double cost = joinCost(alg, outer, inner, joinMemory) + (accessPath(outer) ? 0 : outer.total) + (accessPath(inner) ? 0 : inner.total);
                        candidates.push_back({rest, int(r), alg, cost, joining});
                    }
                }
                if (connected) candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const Candidate& c) { return c.preds.empty(); }), candidates.end());
                size_t winner = 0;
                for (size_t c = 1; c < candidates.size(); c++) if (candidates[c].cost < candidates[winner].cost) winner = c;
                const Candidate& w = candidates[winner];
                LogicalNode j = logicalNode(w.preds.empty() ? "cross" : "join", {best[w.outer], leaves[size_t(w.inner)].node});
                std::vector<std::string> conds;
                for (int p : w.preds) conds.push_back(preds[size_t(p)].text);
                if (!conds.empty()) j.condition = conjunction(conds);
                j.algorithm = w.algorithm;
                j.rows = rowsOut(rows);
                std::vector<ColumnInfo> cols;
                for (size_t i = 0; i < count; i++) if (mask >> i & 1) cols.insert(cols.end(), leaves[i].cols.begin(), leaves[i].cols.end());
                j.pages = pagesFor(j.rows, cols);
                j.cost = joinCost(w.algorithm, j.kids[0], j.kids[1], joinMemory);
                j.total = w.cost;
                for (size_t c = 0; c < candidates.size(); c++) {
                    const Candidate& k = candidates[c];
                    dpTable.push_back(subsetName(mask, leaves) + "  rows=" + std::to_string((long long)j.rows) + " pages=" + std::to_string((long long)j.pages) + "  " + (c == winner ? "* " : "  ") +
                                      subsetName(k.outer, leaves) + " outer, " + k.algorithm + " with " + leaves[size_t(k.inner)].name + " inner" + "  cost=" + std::to_string((long long)std::llround(k.cost)));
                }
                best[mask] = std::move(j);
            }
        }
        LogicalNode result = std::move(best[all]);
        if (!above.empty()) result = wrapSelect(std::move(result), conjunction(above));
        return result;
    }
    LogicalNode reorder(LogicalNode n, int memory) {
        bool block = n.op == "join" || n.op == "cross";
        if (n.op == "select") {
            const LogicalNode* below = &n.kids[0];
            while (below->op == "select") below = &below->kids[0];
            block = below->op == "join" || below->op == "cross";
        }
        if (block) return joinBlock(std::move(n), memory);
        for (LogicalNode& k : n.kids) k = reorder(std::move(k), memoryBelow(n, memory));
        return n;
    }

    // ---- rule 6, projections pushed down to the inputs of joins and grouping; rule 7, tidying ----
    LogicalNode projectOnto(LogicalNode n, const std::vector<Col>& keep) {
        std::vector<Col> have = outputCols(n);
        if (keep.size() == have.size()) return n;
        LogicalNode p = unaryNode("project", std::move(n));
        if (keep.empty()) p.columns.emplace_back(qualified(have[0]), "");
        for (const Col& c : keep) p.columns.emplace_back(qualified(c), "");
        return p;
    }
    static void addNeeded(std::vector<Col>& needed, const Col& c) { if (std::none_of(needed.begin(), needed.end(), [&](const Col& x) { return sameColumn(x, c); })) needed.push_back(c); }
    // the columns of cols that are needed, in the order cols has them
    static std::vector<Col> keepOnly(const std::vector<Col>& cols, const std::vector<Col>& needed) {
        std::vector<Col> out;
        for (const Col& c : cols) if (std::any_of(needed.begin(), needed.end(), [&](const Col& x) { return sameColumn(x, c); })) out.push_back(c);
        return out;
    }
    std::vector<Col> referenced(const std::string& cond, const std::vector<Col>& cols) {
        std::vector<std::string> names;
        attrs(parseCondition(cond), names);
        std::vector<Col> out;
        for (const std::string& name : names) { Node a = attrNode(name); addNeeded(out, cols[resolve(cols, a)]); }
        return out;
    }
    LogicalNode prune(LogicalNode n, std::vector<Col> needed) {
        if (n.kids.empty()) return n;
        std::vector<Col> in = outputCols(n.kids[0]);
        if (n.op == "project") {
            std::vector<Col> want;
            for (const auto& [src, alias] : n.columns) { Node a = attrNode(src); addNeeded(want, in[resolve(in, a)]); }
            n.kids[0] = prune(std::move(n.kids[0]), keepOnly(in, want));
        }
        else if (n.op == "select") { for (const Col& c : referenced(n.condition, in)) addNeeded(needed, c); n.kids[0] = prune(std::move(n.kids[0]), keepOnly(in, needed)); }
        else if (n.op == "sort") {
            for (const auto& [k, desc] : n.sortKeys) { Node a = attrNode(k); addNeeded(needed, in[resolve(in, a)]); }
            n.kids[0] = prune(std::move(n.kids[0]), keepOnly(in, needed));
        }
        else if (n.op == "limit") n.kids[0] = prune(std::move(n.kids[0]), keepOnly(in, needed));
        else if (n.op == "rename") {
            std::vector<Col> want;
            for (const Col& c : needed) for (const Col& x : in) if (x.name == c.name) addNeeded(want, x);
            n.kids[0] = prune(std::move(n.kids[0]), keepOnly(in, want));
        }
        else if (n.op == "group_by") {
            std::vector<Col> want;
            for (const std::string& k : n.keys) { Node a = attrNode(k); addNeeded(want, in[resolve(in, a)]); }
            for (const auto& a : n.aggregates) if (a[1] != "*") { Node x = attrNode(a[1]); addNeeded(want, in[resolve(in, x)]); }
            std::vector<Col> keep = keepOnly(in, want);
            n.kids[0] = prune(std::move(n.kids[0]), keep);
            if (on(6)) n.kids[0] = projectOnto(std::move(n.kids[0]), keep);
        }
        else if (n.op == "join" || n.op == "cross") {
            std::vector<Col> both = in, right = outputCols(n.kids[1]);
            both.insert(both.end(), right.begin(), right.end());
            if (n.op == "join") for (const Col& c : referenced(n.condition, both)) addNeeded(needed, c);
            for (size_t side = 0; side < 2; side++) {
                std::vector<Col> keep = keepOnly(side == 0 ? in : right, needed);
                n.kids[side] = prune(std::move(n.kids[side]), keep);
                if (on(6)) n.kids[side] = projectOnto(std::move(n.kids[side]), keep);
            }
        }
        else for (LogicalNode& k : n.kids) { std::vector<Col> all = outputCols(k); k = prune(std::move(k), all); }
        return n;
    }
    LogicalNode tidy(LogicalNode n) {
        for (LogicalNode& k : n.kids) k = tidy(std::move(k));
        if (!on(7)) return n;
        if (n.op == "project") {
            std::vector<Col> in = outputCols(n.kids[0]);
            bool same = n.columns.size() == in.size();
            for (size_t i = 0; same && i < in.size(); i++) same = n.columns[i].second.empty() && n.columns[i].first == qualified(in[i]);
            if (same) return std::move(n.kids[0]);
        }
        if (n.op == "select" && n.kids[0].op == "select") {
            std::vector<std::string> parts;
            for (const std::string& cond : {n.condition, n.kids[0].condition}) for (const Node& c : conjuncts(parseCondition(cond))) parts.push_back(condText(c));
            LogicalNode merged = std::move(n.kids[0]);
            merged.condition = conjunction(parts);
            return merged;
        }
        return n;
    }

    LogicalNode optimize(LogicalNode n) {
        n = pushSelects(std::move(n));
        n = reorder(std::move(n), frames);
        std::vector<Col> all = outputCols(n);
        n = prune(std::move(n), all);
        n = tidy(std::move(n));
        estimate(n, frames);
        return n;
    }
    // no optimizer at all: joins as written, nested loops throughout, sort based grouping, nothing moved
    LogicalNode baseline(LogicalNode n) {
        for (LogicalNode& k : n.kids) k = baseline(std::move(k));
        if (n.op == "join" || n.op == "cross") n.algorithm = "nested_loop_join";
        if (n.op == "group_by" || n.op == "distinct") n.algorithm = "sort";
        return n;
    }
};

// ---- the physical plan Module 2 runs, and explain ----

inline Json physicalJson(const LogicalNode& n) {
    Json j = jsonObject();
    static const std::map<std::string, std::string> NAMES = {{"scan", "seqscan"}, {"select", "filter"}};
    std::string op = n.op == "join" || n.op == "cross" ? n.algorithm : NAMES.count(n.op) ? NAMES.at(n.op) : n.op;
    j.add("op", jsonString(op));
    if (n.op == "scan") j.add("table", jsonString(n.table));
    if (n.op == "rename") j.add("to", jsonString(n.to));
    if (n.op == "select" || n.op == "join") j.add("condition", jsonString(n.condition));
    if (n.op == "group_by" || n.op == "distinct") j.add("mode", jsonString(n.algorithm));
    Json logical = logicalJson(n);
    for (const char* field : {"columns", "keys", "aggregates", "n"}) if (const Json* v = logical.find(field)) j.add(field, *v);
    j.add("est_rows", jsonNumber((long long)n.rows)).add("est_cost", jsonNumber((long long)std::llround(n.cost)));
    if (n.kids.size() == 1) j.add("child", physicalJson(n.kids[0]));
    if (n.kids.size() == 2) j.add("left", physicalJson(n.kids[0])).add("right", physicalJson(n.kids[1]));
    return j;
}
inline std::string physicalLabel(const LogicalNode& n) {
    std::string label = logicalLabel(n);
    std::string op = n.op == "join" || n.op == "cross" ? n.algorithm : n.op == "scan" ? "seqscan" : n.op == "select" ? "filter" : n.op;
    std::string rest = label.substr(n.op.size());
    if (n.op == "select" || n.op == "join") rest = " (" + n.condition + ")";
    if (n.op == "group_by" || n.op == "distinct") rest = " " + n.algorithm + rest;
    return op + rest;
}
inline void explainLines(const LogicalNode& n, int depth, std::vector<std::pair<std::string, const LogicalNode*>>& lines) {
    lines.emplace_back(std::string(size_t(2 * depth), ' ') + physicalLabel(n), &n);
    for (const LogicalNode& k : n.kids) explainLines(k, depth + 1, lines);
}
inline void printExplain(const LogicalNode& root, std::ostream& out) {
    std::vector<std::pair<std::string, const LogicalNode*>> lines;
    explainLines(root, 0, lines);
    size_t width = 0;
    for (const auto& l : lines) width = std::max(width, l.first.size());
    for (const auto& [text, n] : lines) {
        std::string rows = "est_rows=" + std::to_string((long long)n->rows);
        out << text << std::string(width + 2 - text.size(), ' ') << rows << std::string(rows.size() < 18 ? 18 - rows.size() : 1, ' ') << "est_cost=" << std::llround(n->cost) << '\n';
    }
    out << "total est_cost=" << std::llround(root.total) << '\n';
}
