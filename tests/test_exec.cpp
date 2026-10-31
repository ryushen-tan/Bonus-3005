// Module 2: worked examples, the checklist, and every operator checked against the Bonus 1 engine
#include "../src/plan.hpp"
#include "../src/treejson.hpp"
#include <functional>
#include <sstream>

const std::string DIR = "tests/tmp-exec";

std::string q(const std::string& s) { return jsonQuote(s); }
std::string scan(const std::string& t) { return "{\"op\":\"seqscan\",\"table\":" + q(t) + "}"; }
std::string filter(const std::string& c, const std::string& child) { return "{\"op\":\"filter\",\"condition\":" + q(c) + ",\"child\":" + child + "}"; }
std::string project(const std::string& cols, const std::string& child) { return "{\"op\":\"project\",\"columns\":" + cols + ",\"child\":" + child + "}"; }
std::string rename(const std::string& to, const std::string& child) { return "{\"op\":\"rename\",\"to\":" + q(to) + ",\"child\":" + child + "}"; }
std::string join(const std::string& alg, const std::string& c, const std::string& l, const std::string& r) {
    return "{\"op\":" + q(alg) + (c.empty() ? "" : ",\"condition\":" + q(c)) + ",\"left\":" + l + ",\"right\":" + r + "}";
}
std::string unary(const std::string& op, const std::string& fields, const std::string& child) { return "{\"op\":" + q(op) + (fields.empty() ? "" : "," + fields) + ",\"child\":" + child + "}"; }
std::string binary(const std::string& op, const std::string& l, const std::string& r) { return "{\"op\":" + q(op) + ",\"left\":" + l + ",\"right\":" + r + "}"; }

// the database is declared first so it outlives the operators that hold its frames
struct Outcome { std::unique_ptr<Database> db; std::unique_ptr<Operator> root; std::vector<Row> rows; int peak = 0; int held = 0; bool tempFilesLeft = false; std::string error; };
Outcome runPlan(const std::string& plan, int frames) {
    Outcome o;
    try {
        o.db = std::make_unique<Database>(DIR, frames);
        o.root = buildPlan(*o.db, parseJson(plan));
        Result r = execute(*o.db, *o.root);
        o.rows = r.rows;
        o.peak = r.peakFrames;
        o.held = o.db->pool.held;
        for (const auto& entry : std::filesystem::directory_iterator(DIR)) o.tempFilesLeft = o.tempFilesLeft || entry.path().filename().string().rfind(".tmp", 0) == 0;
    } catch (const Error& e) { o.error = e.msg; }
    return o;
}

// the physical plan as a Bonus 1 relational algebra tree, evaluated by the Bonus 1 engine
Json logical(const Json& j) {
    static const std::map<std::string, std::string> NAMES = {{"seqscan", "scan"}, {"filter", "select"}, {"nested_loop_join", "join"}, {"hash_join", "join"}, {"sort_merge_join", "join"}};
    Json out = jsonObject();
    for (const auto& [k, v] : j.fields) {
        if (k == "op") { auto it = NAMES.find(v.text); std::string op = it == NAMES.end() ? v.text : it->second; out.add("op", jsonString(op == "join" && !j.find("condition") ? "cross" : op)); }
        else if (k == "mode") continue;
        else if (v.kind == Json::Object) out.add(k, logical(v));
        else out.add(k, v);
    }
    return out;
}
std::vector<Row> oracle(const std::string& plan) {
    Database db(DIR, 8);
    std::map<std::string, Rel> rels;
    for (Table& t : db.tables) {
        Rel& r = rels[t.name];
        r.cols = t.cols;
        HeapScan s(db.pool, t.file);
        Rid rid;
        std::string_view rec;
        while (s.next(rid, rec)) r.rows.push_back(decode(t.cols, rec));
    }
    Engine e;
    e.source = [&](const std::string& name) -> const Rel* { auto it = rels.find(name); return it == rels.end() ? nullptr : &it->second; };
    Node tree = jsonToTree(logical(parseJson(plan)));
    return e.exec(tree).rows;
}
bool sameBag(std::vector<Row> a, std::vector<Row> b) {
    if (a.size() != b.size()) return false;
    sortRows(a);
    sortRows(b);
    for (size_t i = 0; i < a.size(); i++) if (!same(a[i], b[i])) return false;
    return true;
}
std::string show(const std::vector<Row>& rows, size_t limit = 8) {
    std::string s;
    for (size_t i = 0; i < rows.size() && i < limit; i++) s += "(" + rowText(rows[i]) + ") ";
    return s + "[" + std::to_string(rows.size()) + " rows]";
}
const Operator& node(const Operator& root, const std::string& op) {
    if (root.op == op) return root;
    for (const auto& c : root.children) if (const Operator* n = &node(*c, op); n->op == op) return *n;
    return root;
}

void setup() {
    std::filesystem::remove_all(DIR);
    Database db(DIR, 16);
    auto table = [&](const std::string& name, const std::string& schema, const std::vector<std::string>& lines) {
        Table& t = db.createTable(name, parseSchema(schema));
        std::string text;
        for (const std::string& l : lines) text += l + "\n";
        loadCsv(db, t, text);
    };
    uint64_t seed = 7;
    auto rnd = [&](int n) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return int((seed >> 33) % uint64_t(n)); };
    std::vector<std::string> r, s, t, skewL, skewR, sorted8, wide, pairs;
    const char* names[] = {"ann", "bob", "cy", "dee", "ed"};
    for (int i = 0; i < 3000; i++) r.push_back(std::to_string(i % 1500) + "," + std::to_string(rnd(40)) + "," + names[rnd(5)]);
    for (int i = 0; i < 1200; i++) s.push_back(std::to_string(rnd(50)) + ",c" + std::to_string(rnd(300)));
    for (int i = 0; i < 500; i++) t.push_back(std::to_string(rnd(60)) + "," + std::to_string(rnd(9)));
    for (int i = 0; i < 4000; i++) { skewL.push_back(std::to_string(i % 5 < 2 ? 7 : rnd(1000)) + "," + std::to_string(i)); skewR.push_back(std::to_string(i % 5 < 2 ? 7 : rnd(1000)) + ",x" + std::to_string(i)); }
    for (int i = 0; i < 1920; i++) sorted8.push_back(std::to_string(rnd(100000)) + ",bob," + std::to_string(i));
    for (int i = 0; i < 3000; i++) wide.push_back(std::to_string(rnd(1000)) + "," + std::string(150, char('a' + rnd(26))));
    table("R", "a:INT,b:INT,s:VARCHAR(10)", r);
    table("S", "b:INT,c:VARCHAR(12)", s);
    table("T", "b:INT,d:INT", t);
    table("SkewL", "k:INT,v:INT", skewL);
    table("SkewR", "k:INT,w:VARCHAR(10)", skewR);
    table("Eight", "a:INT,b:VARCHAR(20),c:INT", sorted8);
    table("Wide", "k:INT,pad:VARCHAR(200)", wide);
    table("Empty", "b:INT,x:VARCHAR(5)", {});
    table("R1", "b:INT", {"1", "2", "2", "3"});
    table("S1", "b:INT", {"2", "2", "4"});
}

struct Test { const char* name; std::function<std::string()> run; };
const std::vector<std::string> ALGS = {"nested_loop_join", "hash_join", "sort_merge_join"};

// plans whose result the Bonus 1 engine can compute, each run at several budgets
std::vector<std::string> oraclePlans() {
    std::vector<std::string> plans;
    for (const std::string& alg : ALGS) {
        plans.push_back(join(alg, "R.b = S.b", scan("R"), scan("S")));
        plans.push_back(join(alg, "R.b = S.b", filter("R.a > 700 and R.s != 'bob'", scan("R")), scan("S")));
        plans.push_back(project("[\"R.a\",\"T.d\"]", join(alg, "S.b = T.b", join(alg, "R.b = S.b", scan("R"), scan("S")), scan("T"))));
        plans.push_back(join(alg, "x.b = y.b", rename("x", scan("T")), rename("y", scan("T"))));
        plans.push_back(join(alg, "SkewL.k = SkewR.k", scan("SkewL"), scan("SkewR")));
        plans.push_back(join(alg, "R1.b = S1.b", scan("R1"), scan("S1")));
        plans.push_back(join(alg, "R.b = Empty.b", scan("R"), scan("Empty")));
        plans.push_back(join(alg, "Empty.b = R.b", scan("Empty"), scan("R")));
        plans.push_back(join(alg, "R.b = S.b and R.a = S.b", scan("R"), scan("S")));
        plans.push_back(join(alg, "a.k = b.k", rename("a", scan("Wide")), rename("b", scan("Wide"))));
        plans.push_back(join(alg, "R.b = T.b", unary("distinct", "\"mode\":\"hash\"", project("[\"R.b\",\"R.s\"]", scan("R"))), scan("T")));
        plans.push_back(join(alg, "R.b = T.b", scan("T"), unary("distinct", "\"mode\":\"sort\"", project("[\"R.b\"]", scan("R")))));
    }
    plans.push_back(join("nested_loop_join", "R.b < T.b and T.d = 3", scan("R"), scan("T")));
    plans.push_back(join("nested_loop_join", "", scan("T"), scan("S1")));
    for (const char* mode : {"sort", "hash"}) {
        std::string m = std::string("\"mode\":\"") + mode + "\"";
        plans.push_back(unary("distinct", m, project("[\"R.b\",\"R.s\"]", scan("R"))));
        plans.push_back(unary("distinct", m, scan("Wide")));
        plans.push_back(unary("distinct", m, scan("Empty")));
        plans.push_back(unary("distinct", m, join("hash_join", "R.b = S.b", scan("R"), scan("S"))));
    }
    for (const char* op : {"union", "intersect", "except"}) {
        plans.push_back(binary(op, project("[\"R.b\"]", scan("R")), project("[\"T.b\"]", scan("T"))));
        plans.push_back(binary(op, project("[\"T.b\"]", scan("T")), project("[\"R.b\"]", scan("R"))));
        plans.push_back(binary(op, project("[\"R.b\"]", scan("R")), project("[\"Empty.b\"]", scan("Empty"))));
        plans.push_back(binary(op, project("[\"Empty.b\"]", scan("Empty")), project("[\"R.b\"]", scan("R"))));
        plans.push_back(binary(op, scan("Wide"), filter("Wide.k < 500", scan("Wide"))));
        plans.push_back(binary(op, project("[\"R.b\"]", join("hash_join", "R.b = S.b", scan("R"), scan("S"))), unary("distinct", "\"mode\":\"hash\"", project("[\"T.b\"]", scan("T")))));
    }
    return plans;
}

const std::vector<Test> TESTS = {
    {"every oracle plan matches the Bonus 1 engine at B = 4, 5, 8 and 64, leaving nothing pinned and no files", [] {
        std::string problems;
        for (const std::string& plan : oraclePlans()) {
            std::vector<Row> want = oracle(plan);
            for (int b : {4, 5, 8, 64}) {
                Outcome o = runPlan(plan, b);
                if (!o.error.empty() || !sameBag(o.rows, want) || o.held != 0 || o.tempFilesLeft || o.peak > b)
                    problems += "B=" + std::to_string(b) + " " + plan + "\n  " + (o.error.empty() ? show(o.rows) + " want " + show(want) + " peak " + std::to_string(o.peak) + " held " + std::to_string(o.held) : o.error) + "\n";
            }
        }
        return problems;
    }},
    {"2.5 sort example: N = 8, B = 3 gives 3 passes, 22 reads and 14 writes", [] {
        Outcome o = runPlan(unary("sort", "\"keys\":[[\"Eight.a\",\"desc\"]]", scan("Eight")), 3);
        const Operator& sort = *o.root;
        const Operator& sc = *sort.children[0];
        bool ordered = std::is_sorted(o.rows.begin(), o.rows.end(), [](const Row& x, const Row& y) { return x[0].num > y[0].num; });
        std::string got = "sort reads=" + std::to_string(sort.counters.io.reads) + " writes=" + std::to_string(sort.counters.io.writes) + " " + sort.notes +
                          " scan reads=" + std::to_string(sc.counters.io.reads) + " rows=" + std::to_string(o.rows.size());
        return o.error.empty() && ordered && got == "sort reads=14 writes=14 passes=3 scan reads=9 rows=1920" ? "" : got + o.error;
    }},
    {"2.8 duplicate keys: [1,2,2,3] join [2,2,4] is four rows for every join", [] {
        std::string got;
        for (const std::string& alg : ALGS) got += std::to_string(runPlan(join(alg, "R1.b = S1.b", scan("R1"), scan("S1")), 4).rows.size()) + " ";
        return got == "4 4 4 " ? "" : got;
    }},
    {"block nested loop compares exactly |R| x |S| pairs, at every B", [] {
        std::string got;
        for (int b : {4, 7, 32}) {
            Outcome o = runPlan(join("nested_loop_join", "R.b = S.b", scan("R"), scan("S")), b);
            got += std::to_string(o.root->counters.comparisons) + " ";
        }
        return got == "3600000 3600000 3600000 " ? "" : got;
    }},
    {"block nested loop reads M + ceil(M / (B - 2)) * N data pages, plus a directory page per scan", [] {
        std::string problems;
        long long m, n;
        {
            Database db(DIR, 4);
            m = db.table("R").dataPages;
            n = db.table("S").dataPages;
        }
        for (int b : {4, 5, 8, 16}) {
            Outcome o = runPlan(join("nested_loop_join", "R.b = S.b", scan("R"), scan("S")), b);
            long long blocks = (m + b - 3) / (b - 2);
            long long want = (m + 1) + blocks * (n + 1);
            long long got = (long long)(o.root->children[0]->counters.io.reads + o.root->children[1]->counters.io.reads);
            if (got != want) problems += "B=" + std::to_string(b) + " reads " + std::to_string(got) + " want " + std::to_string(want) + "\n";
        }
        return problems;
    }},
    {"a join whose output is larger than both inputs completes", [] {
        Outcome o = runPlan(join("hash_join", "x.k = y.k", rename("x", scan("SkewL")), rename("y", scan("SkewR"))), 4);
        return o.error.empty() && o.rows.size() > 2000000 ? "" : o.error + std::to_string(o.rows.size());
    }},
    {"the skewed hash join falls back to block nested loop for the hot partition", [] {
        Outcome o = runPlan(join("hash_join", "SkewL.k = SkewR.k", scan("SkewL"), scan("SkewR")), 4);
        return o.root->notes.find("overflowed=1") != std::string::npos ? "" : o.root->notes;
    }},
    {"a sort of twenty times B pages runs at B = 4 and never holds more than 4 frames", [] {
        Outcome o = runPlan(unary("sort", "\"keys\":[[\"Wide.pad\",\"asc\"],[\"Wide.k\",\"desc\"]]", scan("Wide")), 4);
        bool ordered = std::is_sorted(o.rows.begin(), o.rows.end(), [](const Row& x, const Row& y) { int d = compare(x[1], y[1]); return d ? d < 0 : x[0].num > y[0].num; });
        size_t pages = o.root->children[0]->counters.io.reads;
        return o.error.empty() && ordered && o.rows.size() == 3000 && pages >= 81 && o.peak == 4 ? "" : o.root->notes + " pages " + std::to_string(pages) + " peak " + std::to_string(o.peak) + o.error;
    }},
    {"an empty input through every operator", [] {
        std::string e = scan("Empty"), got;
        for (const std::string& plan : {filter("Empty.b > 1", e), project("[\"Empty.x\"]", e), rename("z", e), unary("limit", "\"n\":3", e),
                                        unary("sort", "\"keys\":[[\"Empty.b\",\"asc\"]]", e), unary("distinct", "\"mode\":\"sort\"", e), unary("distinct", "\"mode\":\"hash\"", e),
                                        unary("group_by", "\"mode\":\"sort\",\"keys\":[\"Empty.b\"],\"aggregates\":[[\"count\",\"*\"]]", e),
                                        unary("group_by", "\"mode\":\"hash\",\"keys\":[\"Empty.b\"],\"aggregates\":[[\"count\",\"*\"]]", e),
                                        binary("union_all", e, e), binary("union", e, e), binary("intersect", e, e), binary("except", e, e)}) {
            Outcome o = runPlan(plan, 4);
            got += std::to_string(o.rows.size()) + o.error;
        }
        for (const char* mode : {"sort", "hash"}) {
            Outcome o = runPlan(unary("group_by", std::string("\"mode\":\"") + mode + "\",\"keys\":[],\"aggregates\":[[\"count\",\"*\"],[\"count\",\"Empty.x\",\"n\"]]", e), 4);
            got += " " + show(o.rows) + o.error;
            o = runPlan(unary("group_by", std::string("\"mode\":\"") + mode + "\",\"keys\":[],\"aggregates\":[[\"sum\",\"Empty.b\"]]", e), 4);
            got += " " + o.error;
        }
        std::string undefined = " error: sum of an empty input with no grouping columns is undefined";
        return got == "0000000000000 (0, 0) [1 rows]" + undefined + " (0, 0) [1 rows]" + undefined ? "" : got;
    }},
    {"group_by aggregates agree in both modes and with a direct count", [] {
        std::string plan = "\"keys\":[\"R.s\"],\"aggregates\":[[\"count\",\"*\"],[\"sum\",\"R.b\"],[\"avg\",\"R.b\",\"mean\"],[\"min\",\"R.a\"],[\"max\",\"R.a\"],[\"min\",\"R.s\"]]";
        Outcome sorted = runPlan(unary("group_by", "\"mode\":\"sort\"," + plan, scan("R")), 4), hashed = runPlan(unary("group_by", "\"mode\":\"hash\"," + plan, scan("R")), 4);
        std::vector<Row> all = oracle(scan("R"));
        long long count = 0, sum = 0;
        for (const Row& r : all) if (r[2].text == "bob") { count++; sum += (long long)r[1].num; }
        char mean[32];
        std::snprintf(mean, sizeof mean, "%.4f", double(sum) / double(count));
        std::string bob;
        for (const Row& r : sorted.rows) if (r[0].text == "bob") bob = rowText(r);
        std::string want = "bob, " + std::to_string(count) + ", " + std::to_string(sum) + ", " + mean;
        std::string names;
        for (const Col& c : sorted.root->schema) names += qualified(c) + " ";
        return sameBag(sorted.rows, hashed.rows) && sorted.rows.size() == 5 && bob.rfind(want, 0) == 0 && names == "R.s count_star sum_R_b mean min_R_a max_R_a min_R_s " ? "" : bob + " | " + want + " | " + names;
    }},
    {"hash group_by with far more groups than memory spills and recurses, and still agrees", [] {
        std::string plan = "\"keys\":[\"Wide.k\",\"Wide.pad\"],\"aggregates\":[[\"count\",\"*\"]]";
        Outcome hashed = runPlan(unary("group_by", "\"mode\":\"hash\"," + plan, scan("Wide")), 4), sorted = runPlan(unary("group_by", "\"mode\":\"sort\"," + plan, scan("Wide")), 4);
        return hashed.error.empty() && sameBag(hashed.rows, sorted.rows) && hashed.root->notes.find("spilled_pages=[") != std::string::npos ? "" : hashed.root->notes + hashed.error;
    }},
    {"a group_by over a hash join at B = 4 materialises the join first", [] {
        Outcome o = runPlan(unary("group_by", "\"mode\":\"hash\",\"keys\":[\"S.c\"],\"aggregates\":[[\"count\",\"*\"]]", join("hash_join", "R.b = S.b", scan("R"), scan("S"))), 4);
        return o.error.empty() && o.root->counters.io.writes > 0 && o.peak == 4 && o.held == 0 ? "" : o.error;
    }},
    {"union_all keeps duplicates and opens its right input only after the left", [] {
        Outcome o = runPlan(binary("union_all", project("[\"R1.b\"]", scan("R1")), project("[\"S1.b\"]", scan("S1"))), 4);
        return show(o.rows) == "(1) (2) (2) (3) (2) (2) (4) [7 rows]" && o.peak == 1 ? "" : show(o.rows) + std::to_string(o.peak);
    }},
    {"set operators are sets and take the left names", [] {
        std::string got;
        for (const char* op : {"union", "intersect", "except"}) {
            Outcome o = runPlan(binary(op, project("[\"S1.b\"]", scan("S1")), project("[\"R1.b\"]", scan("R1"))), 4);
            got += std::string(op) + " " + qualified(o.root->schema[0]) + " " + show(o.rows) + "\n";
        }
        return got == "union S1.b (1) (2) (3) (4) [4 rows]\nintersect S1.b (2) [1 rows]\nexcept S1.b (4) [1 rows]\n" ? "" : got;
    }},
    {"limit stops pulling from its child", [] {
        Outcome o = runPlan(unary("limit", "\"n\":2", scan("Wide")), 4);
        return o.rows.size() == 2 && o.root->children[0]->counters.tuplesOut == 2 && o.root->children[0]->counters.io.reads == 2 ? "" : "read " + std::to_string(o.root->children[0]->counters.io.reads);
    }},
    {"projection aliases and qualified output", [] {
        Outcome o = runPlan(rename("t", project("[[\"R.a\",\"x\"],\"R.s\"]", scan("R"))), 4);
        return o.error.empty() && qualified(o.root->schema[0]) == "t.x" && qualified(o.root->schema[1]) == "t.s" ? "" : o.error;
    }},
    {"running the same plan twice gives identical counters", [] {
        std::string plan = unary("sort", "\"keys\":[[\"S.c\",\"asc\"]]", unary("group_by", "\"mode\":\"hash\",\"keys\":[\"S.c\"],\"aggregates\":[[\"count\",\"*\"]]",
                                 join("sort_merge_join", "R.b = S.b", scan("R"), scan("S"))));
        Outcome a = runPlan(plan, 5), b = runPlan(plan, 5);
        std::ostringstream ta, tb;
        printTrace(*a.root, ta);
        printTrace(*b.root, tb);
        return ta.str() == tb.str() && a.rows.size() == b.rows.size() ? "" : ta.str() + tb.str();
    }},
    {"plans that do not conform are rejected naming the node", [] {
        std::string got;
        for (const std::string& plan : {std::string("{\"op\":\"sort\",\"child\":") + scan("R") + "}",
                                        std::string("{\"op\":\"filter\",\"condition\":3,\"child\":") + scan("R") + "}",
                                        std::string("{\"op\":\"project\",\"columns\":[\"R.a\"],\"child\":{\"op\":\"merge_join\",\"left\":") + scan("R") + ",\"right\":" + scan("S") + "}}",
                                        std::string("{\"op\":\"distinct\",\"mode\":\"fast\",\"child\":") + scan("R") + "}",
                                        std::string("{\"op\":\"limit\",\"n\":-1,\"child\":") + scan("R") + "}",
                                        std::string("{\"op\":\"group_by\",\"mode\":\"hash\",\"keys\":[],\"aggregates\":[[\"sum\",\"*\"]],\"child\":") + scan("R") + "}",
                                        std::string("{\"child\":") + scan("R") + "}",
                                        std::string("[1]"),
                                        std::string("{\"op\":\"filter\",\"condition\":\"R.a > 1\",\"child\":{\"op\":\"seqscan\",\"table\":\"R\"}"),
                                        std::string("{\"op\":\"seqscan\",\"table\":\"R\",}")})
            got += runPlan(plan, 4).error + "\n";
        std::string want =
            "Node 'sort' (root): missing required field 'keys'.\n"
            "Node 'filter' (root): field 'condition' must be a non-empty string.\n"
            "Node 'merge_join' (root.child): unknown operator 'merge_join'.\n"
            "Node 'distinct' (root): field 'mode' must be \"sort\" or \"hash\".\n"
            "Node 'limit' (root): field 'n' must be a non-negative integer.\n"
            "Node 'group_by' (root): field 'aggregates' must be an array of [function, column] or [function, column, name] with function count, sum, avg, min or max, and * only for count.\n"
            "Node at root: missing required field 'op'.\n"
            "Node at root: must be an object, not an array.\n"
            "json syntax error: expected ',' or '}' but found end of input\n"
            "json syntax error: expected a string key but found '}'\n";
        return got == want ? "" : got;
    }},
    {"unknown tables and columns are rejected before anything runs", [] {
        std::string got;
        for (const std::string& plan : {scan("Nope"), filter("R.zz > 1", scan("R")), project("[\"b\"]", join("hash_join", "R.b = S.b", scan("R"), scan("S"))),
                                        join("hash_join", "R.b < S.b", scan("R"), scan("S")), join("hash_join", "R.b = S.c", scan("R"), scan("S")),
                                        join("nested_loop_join", "", scan("R"), scan("R")), binary("union", scan("R"), scan("S")), filter("R.a > ", scan("R"))}) {
            Outcome o = runPlan(plan, 4);
            got += o.error + "\n";
        }
        std::string want =
            "Node 'seqscan' (root): unknown table 'Nope'.\n"
            "Node 'filter' (root): name error: unknown attribute R.zz in condition 'R.zz > 1'.\n"
            "Node 'project' (root): name error: ambiguous attribute b.\n"
            "Node 'hash_join' (root): condition 'R.b < S.b' is not an equality between a column of each input in condition 'R.b < S.b'.\n"
            "Node 'hash_join' (root): type error: cannot compare number R.b with string S.c in condition 'R.b = S.c'.\n"
            "Node 'nested_loop_join' (root): schema error: duplicate attribute R.a in nested_loop_join.\n"
            "Node 'union' (root): inputs have 3 and 2 columns.\n"
            "Node 'filter' (root): syntax error: expected attribute name but found end of input in condition 'R.a > '.\n";
        return got == want ? "" : got;
    }},
};

int main() {
    setup();
    int fails = 0;
    for (const Test& t : TESTS) {
        std::string problem;
        try { problem = t.run(); } catch (const Error& e) { problem = "threw " + e.msg; }
        if (problem.empty()) continue;
        fails++;
        std::cout << "FAIL " << t.name << "\n" << problem << "\n";
    }
    std::filesystem::remove_all(DIR);
    std::cout << TESTS.size() - fails << "/" << TESTS.size() << " passed\n";
    return fails != 0;
}
