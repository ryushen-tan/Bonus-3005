// Module 4: the worked examples, the checklist, SQL run through all four modules, and the db shell
#include "../src/shell.hpp"
#include <functional>
#include <sstream>

const std::string DIR = "tests/tmp-opt";

LogicalNode compileSql(Database& db, const std::string& sql) {
    SqlCompiler c = compilerFor(db);
    return logicalFromJson(c.statementJson(parseSql(sql)[0]));
}
std::string explainText(Database& db, const std::string& sql, int frames, std::set<int> without = {}) {
    Optimizer o{db, frames, without, {}};
    LogicalNode plan = o.optimize(compileSql(db, sql));
    std::ostringstream out;
    printExplain(plan, out);
    return out.str();
}
std::vector<Row> runPlan(Database& db, const LogicalNode& logical, int frames, bool optimized, std::string* physical = nullptr) {
    Optimizer o{db, frames, {}, {}};
    LogicalNode plan = optimized ? o.optimize(logical) : o.baseline(logical);
    if (!optimized) o.estimate(plan, frames);
    Json json = physicalJson(plan);
    if (physical) *physical = dumpJson(json);
    Database run(DIR, frames);
    std::unique_ptr<Operator> root = buildPlan(run, parseJson(dumpJson(json)));
    return execute(run, *root).rows;
}
std::vector<Row> runSql(Database& db, const std::string& sql, int frames, bool optimized, std::string* physical = nullptr) { return runPlan(db, compileSql(db, sql), frames, optimized, physical); }
bool sameBag(std::vector<Row> a, std::vector<Row> b) {
    if (a.size() != b.size()) return false;
    sortRows(a);
    sortRows(b);
    for (size_t i = 0; i < a.size(); i++) if (!same(a[i], b[i])) return false;
    return true;
}
void table(Database& db, const std::string& name, const std::string& schema, const std::vector<std::string>& lines) {
    Table& t = db.createTable(name, parseSchema(schema));
    std::string text;
    for (const std::string& l : lines) text += l + "\n";
    loadCsv(db, t, text);
}

void setup() {
    std::filesystem::remove_all(DIR);
    Database db(DIR, 64);
    uint64_t seed = 11;
    auto rnd = [&](int n) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return int((seed >> 33) % uint64_t(n)); };
    std::vector<std::string> emp, dept, proj, loc;
    const char* cities[] = {"Ottawa", "Toronto", "Montreal", "Kanata"};
    for (int i = 0; i < 3000; i++) emp.push_back(std::to_string(i) + ",e" + std::to_string(i) + "," + std::to_string(20 + rnd(50)) + "," + std::to_string(rnd(40)) + "," + std::to_string(30000 + rnd(90000)) + "," + std::to_string(rnd(3000)) + "," + cities[rnd(4)]);
    for (int i = 0; i < 40; i++) dept.push_back(std::to_string(i) + ",d" + std::to_string(i) + "," + std::to_string(rnd(5000)) + "," + cities[rnd(4)]);
    for (int i = 0; i < 400; i++) proj.push_back(std::to_string(i) + "," + std::to_string(rnd(40)) + ",p" + std::to_string(rnd(100)));
    for (int i = 0; i < 900; i++) loc.push_back(std::to_string(i) + "," + std::to_string(rnd(400)) + "," + cities[rnd(4)]);
    table(db, "Emp", "eid:INT,name:VARCHAR(20),age:INT,did:INT,salary:INT,mgr:INT,city:VARCHAR(20)", emp);
    table(db, "Dept", "did:INT,title:VARCHAR(20),budget:INT,city:VARCHAR(20)", dept);
    table(db, "Proj", "pid:INT,did:INT,name:VARCHAR(20)", proj);
    table(db, "Loc", "lid:INT,pid:INT,place:VARCHAR(20)", loc);
    for (const char* t : {"Emp", "Dept", "Proj", "Loc"}) analyzeTable(db, db.table(t), 20);
    db.createTable("NoStats", parseSchema("k:INT,v:VARCHAR(10)"));
    table(db, "Ten", "v:INT", {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"});
    analyzeTable(db, db.table("Ten"), 3);
    // 19 rows of 210 bytes with their slots fill a page, so these 190 rows are 10 pages, as the estimate says
    std::vector<std::string> wide;
    for (int i = 0; i < 190; i++) wide.push_back(std::to_string(rnd(1000)) + "," + std::string(200, 'w'));
    table(db, "Wide", "k:INT,pad:VARCHAR(200)", wide);
    analyzeTable(db, db.table("Wide"), 20);
}

// the db shell run on text as its standard input: the error lines it prints and its exit code
std::string shellErrors(const std::string& text, const std::vector<std::string>& args) {
    std::istringstream in(text);
    std::ostringstream out;
    int code = shellTool(args, in, out);
    std::istringstream lines(out.str());
    std::string errors;
    for (std::string line; std::getline(lines, line);) if (line.find("error: ") != std::string::npos) errors += line + "\n";
    return errors + "exit " + std::to_string(code);
}
// a new database made through the shell, T(a, b) holding (1, 'x') and (2, 'y')
std::string shellDatabase() {
    std::string dir = DIR + "-shell";
    std::filesystem::remove_all(dir);
    shellErrors("CREATE TABLE T (a INT, b VARCHAR(5));\nINSERT INTO T VALUES (1, 'x');\nINSERT INTO T VALUES (2, 'y');\n", {"--db", dir});
    return dir;
}
// statistics for T.a whose minimum is not a number, so that reading the catalog back throws std::invalid_argument
void corruptStatistics(const std::string& dir) {
    Database db(dir, 8);
    Table& t = db.table("T");
    t.stats.analyzed = true;
    t.stats.columns[0] = {1, textValue("x"), textValue("x"), {}, 4};
    db.saveCatalog();
}
bool oneInternalError(const std::string& out) { return out.compare(0, 16, "internal error: ") == 0 && out.find('\n') == out.size() - 1; }
std::string repeated(const std::string& s, int n) { std::string out; for (int i = 0; i < n; i++) out += s; return out; }
// sql compile, opt plan and exec run chained by files, as tests/e2e/run.py runs them: how many rows came out,
// or what the tool that failed printed
std::string chained(const std::string& sql, std::vector<std::string> optArgs) {
    std::ofstream(DIR + "/chain.sql") << sql;
    std::ostringstream logical, physical, result;
    if (sqlTool({"compile", "--db", DIR, "--file", DIR + "/chain.sql", "--json"}, logical) != 0) return logical.str();
    std::ofstream(DIR + "/chain.json") << logical.str();
    optArgs.insert(optArgs.begin(), {"plan", "--db", DIR, "--logical", DIR + "/chain.json"});
    if (optTool(optArgs, physical) != 0) return physical.str();
    std::ofstream(DIR + "/plan.json") << physical.str();
    if (execTool({"run", "--db", DIR, "--plan", DIR + "/plan.json", "--json"}, result) != 0) return result.str();
    return std::to_string(parseJson(result.str()).find("rows")->items.size()) + " rows";
}

const std::vector<std::string> QUERIES = {
    "SELECT name, age FROM Emp WHERE age > 60;",
    "SELECT e.name, d.title FROM Emp e JOIN Dept d ON e.did = d.did WHERE d.budget > 1000;",
    "SELECT e.name FROM Emp e, Dept d WHERE e.did = d.did AND e.age > 60 AND d.budget > 1000;",
    "SELECT did, COUNT(*) AS n, AVG(salary) AS avg_sal FROM Emp WHERE age > 25 GROUP BY did HAVING COUNT(*) > 5 ORDER BY n DESC LIMIT 10;",
    "SELECT e.name, m.name FROM Emp e JOIN Emp m ON e.mgr = m.eid WHERE e.age < 22;",
    "SELECT t.did, t.n FROM (SELECT did, COUNT(*) AS n FROM Emp GROUP BY did) AS t WHERE t.n > 3 AND t.did < 20;",
    "SELECT city FROM Emp UNION SELECT city FROM Dept;",
    "SELECT city FROM Emp UNION ALL SELECT city FROM Dept;",
    "SELECT city FROM Emp INTERSECT SELECT place FROM Loc;",
    "SELECT name FROM Proj EXCEPT SELECT title FROM Dept;",
    "SELECT e.name, l.place FROM Emp e, Dept d, Proj p, Loc l WHERE e.did = d.did AND d.did = p.did AND p.pid = l.pid AND e.age > 66;",
    "SELECT e.name, l.place FROM Emp e JOIN Dept d ON e.did = d.did JOIN Proj p ON d.did = p.did JOIN Loc l ON p.pid = l.pid WHERE e.age > 66;",
    "SELECT d.title, COUNT(*) FROM Dept d, Proj p WHERE d.did = p.did AND d.city <> p.name GROUP BY d.title;",
    "SELECT DISTINCT city FROM Emp WHERE salary > 100000 OR age < 21;",
    "SELECT COUNT(*) FROM Emp, Dept WHERE Emp.did = Dept.did AND Emp.city = Dept.city;",
    "SELECT COUNT(*) FROM NoStats;",
    "SELECT x.k FROM NoStats x JOIN Emp e ON x.k = e.eid;",
    "SELECT name FROM Emp ORDER BY age DESC, name LIMIT 7;",
    "SELECT e.city, MIN(e.salary), MAX(d.budget) FROM Emp e JOIN Dept d ON e.did = d.did GROUP BY e.city;",
    "SELECT p.name FROM Proj p, Loc l WHERE p.pid < l.pid AND l.lid < 5 AND p.pid < 10;",
    "SELECT d.title FROM Dept d, Loc l WHERE d.did < 3 AND l.lid < 3;",
    "SELECT t.city, COUNT(*) FROM (SELECT city FROM Emp UNION ALL SELECT place FROM Loc) AS t WHERE t.city <> 'Kanata' GROUP BY t.city;",
    "SELECT e.name, m.name FROM Emp e JOIN Emp m ON e.mgr = m.eid;",
    "SELECT COUNT(*) FROM Dept d, Proj p, Loc l WHERE d.did < 5 AND p.pid < 50 AND (d.did = p.did OR p.pid = l.pid);",
    "SELECT did, COUNT(*) AS did FROM Emp GROUP BY did HAVING COUNT(*) > 80;",
};

struct Test { const char* name; std::function<std::string()> run; };
const std::vector<Test> TESTS = {
    {"every query gives the same rows optimized and unoptimized, at B = 4, 8 and 32", [] {
        std::string problems;
        Database db(DIR, 64);
        for (size_t i = 0; i < QUERIES.size(); i++)
            for (int b : {4, 8, 32}) {
                const std::string& q = QUERIES[i];
                // the unoptimized four-way comma join is a cross product of 43 billion rows, so its JOIN ON twin stands in
                const std::string& reference = i == 10 ? QUERIES[11] : q;
                std::string physical;
                try {
                    std::vector<Row> fast = runSql(db, q, b, true, &physical), slow = runSql(db, reference, b, false);
                    if (!sameBag(fast, slow)) problems += "B=" + std::to_string(b) + " " + q + ": " + std::to_string(fast.size()) + " rows against " + std::to_string(slow.size()) + "\n";
                } catch (const Error& e) { problems += "B=" + std::to_string(b) + " " + q + ": " + e.msg + "\n" + physical + "\n"; }
                catch (const SqlError& e) { problems += q + ": " + sqlErrorText(e) + "\n"; }
            }
        return problems;
    }},
    {"section 2 selectivity example: 200, 2000, 40 and 10000 rows", [] {
        std::filesystem::remove_all(DIR + "-sel");
        Database db(DIR + "-sel", 32);
        Table& emp = db.createTable("Emp", parseSchema("did:INT,age:INT"));
        Table& dept = db.createTable("Dept", parseSchema("did:INT"));
        emp.dataPages = 200;
        emp.stats = {true, 10000, 200, 8, {{50, intValue(0), intValue(49), {}, 4}, {51, intValue(20), intValue(70), {}, 4}}};
        dept.dataPages = 1;
        dept.stats = {true, 50, 1, 4, {{50, intValue(0), intValue(49), {}, 4}}};
        std::string got;
        for (const std::string& sql : {"SELECT * FROM Emp WHERE did = 7;", "SELECT * FROM Emp WHERE age > 60;", "SELECT * FROM Emp WHERE did = 7 AND age > 60;", "SELECT * FROM Emp JOIN Dept ON Emp.did = Dept.did;"}) {
            Optimizer o{db, 32, {}, {}};
            LogicalNode plan = o.optimize(compileSql(db, sql));
            got += std::to_string((long long)plan.rows) + " ";
        }
        std::filesystem::remove_all(DIR + "-sel");
        return got == "200 2000 40 10000 " ? "" : got;
    }},
    {"section 3 rewrite example reproduces", [] {
        Database db(DIR, 32);
        Optimizer o{db, 32, {}, {}};
        LogicalNode plan = o.optimize(compileSql(db, "SELECT e.name FROM Emp e, Dept d WHERE e.did = d.did AND e.age > 60 AND d.budget > 1000;"));
        std::ostringstream out;
        printLogical(plan, out);
        std::string want = "project [e.name]\n  join e.did = d.did\n    project [e.name, e.did]\n      select e.age > 60\n        rename e\n          scan Emp\n    project [d.did]\n      select d.budget > 1000\n        rename d\n          scan Dept\n";
        return out.str() == want ? "" : out.str();
    }},
    {"section 5.1 DP table reproduces", [] {
        std::filesystem::remove_all(DIR + "-dp");
        Database db(DIR + "-dp", 32);
        auto give = [&](const std::string& name, const std::string& schema, long long tuples, uint32_t pages, std::vector<ColumnStats> cols) {
            Table& t = db.createTable(name, parseSchema(schema));
            t.dataPages = pages;
            t.stats = {true, tuples, pages, 40, cols};
        };
        give("A", "x:INT,pad:VARCHAR(40)", 5000, 100, {{2500, intValue(0), intValue(2499), {}, 4}, {1, textValue("a"), textValue("a"), {}, 36}});
        give("B", "x:INT,y:INT,pad:VARCHAR(40)", 500, 10, {{500, intValue(0), intValue(499), {}, 4}, {500, intValue(0), intValue(499), {}, 4}, {1, textValue("a"), textValue("a"), {}, 32}});
        give("C", "y:INT,pad:VARCHAR(40)", 20000, 400, {{5000, intValue(0), intValue(4999), {}, 4}, {1, textValue("a"), textValue("a"), {}, 36}});
        Optimizer o{db, 32, {}, {}};
        LogicalNode plan = o.optimize(compileSql(db, "SELECT * FROM A, B, C WHERE A.x = B.x AND B.y = C.y;"));
        std::string got;
        for (const std::string& line : o.dpTable) if (line.find("* ") != std::string::npos || line.find("skipped") != std::string::npos || line.find("{A} outer") != std::string::npos || line.find("{B,C} outer, nested") != std::string::npos) got += line + "\n";
        std::string want =
            "{A}  rows=5000 pages=100  * access path  cost=100\n"
            "{B}  rows=500 pages=10  * access path  cost=10\n"
            "{C}  rows=20000 pages=400  * access path  cost=400\n"
            "{A,B}  rows=1000 pages=20    {A} outer, nested_loop_join with B inner  cost=140\n"
            "{A,B}  rows=1000 pages=20    {A} outer, hash_join with B inner  cost=330\n"
            "{A,B}  rows=1000 pages=20    {A} outer, sort_merge_join with B inner  cost=420\n"
            "{A,B}  rows=1000 pages=20  * {B} outer, nested_loop_join with A inner  cost=110\n"
            "{A,C}  no join predicate connects it, skipped\n"
            "{B,C}  rows=2000 pages=40  * {B} outer, nested_loop_join with C inner  cost=410\n"
            "{A,B,C}  rows=4000 pages=118  * {A,B} outer, nested_loop_join with C inner  cost=530\n"
            "{A,B,C}  rows=4000 pages=118    {B,C} outer, nested_loop_join with A inner  cost=650\n";
        std::filesystem::remove_all(DIR + "-dp");
        return got == want ? "" : got;
    }},
    {"commas and JOIN ON give the same physical plan", [] {
        Database db(DIR, 32);
        std::string a, b;
        runSql(db, QUERIES[10], 32, true, &a);
        runSql(db, QUERIES[11], 32, true, &b);
        std::string c, d;
        runSql(db, QUERIES[2], 32, true, &c);
        runSql(db, "SELECT e.name FROM Emp e JOIN Dept d ON e.did = d.did WHERE e.age > 60 AND d.budget > 1000;", 32, true, &d);
        return a == b && c == d ? "" : a + "\n" + b;
    }},
    {"changing B changes the join algorithm", [] {
        Database db(DIR, 32);
        std::string small = explainText(db, QUERIES[22], 4), large = explainText(db, QUERIES[22], 64);
        return small.find("hash_join") != std::string::npos && large.find("nested_loop_join") != std::string::npos ? "" : small + large;
    }},
    {"ANALYZE twice changes nothing", [] {
        std::string before = readFile(DIR + "/catalog.db");
        {
            Database db(DIR, 64);
            analyzeTable(db, db.table("Emp"), 20);
        }
        return readFile(DIR + "/catalog.db") == before ? "" : "catalog.db changed";
    }},
    {"ANALYZE builds an equi-depth histogram", [] {
        Database db(DIR, 64);
        const ColumnStats& s = db.table("Ten").stats.columns[0];
        std::string got = std::to_string(s.ndv) + " " + s.min.text + " " + s.max.text;
        for (const Bucket& b : s.histogram) got += " [" + b.lo.text + "," + b.hi.text + "] " + std::to_string(b.count) + "/" + std::to_string(b.ndv);
        return got == "10 1 10 [1,4] 4/4 [5,7] 3/3 [8,10] 3/3" ? "" : got;
    }},
    {"ANALYZE statistics are exact", [] {
        Database db(DIR, 64);
        Table& d = db.table("Dept");
        std::vector<Row> rows = runSql(db, "SELECT DISTINCT city FROM Dept;", 32, false);
        return d.stats.tuples == 40 && d.stats.columns[3].ndv == (long long)rows.size() && d.stats.columns[0].ndv == 40 && d.stats.columns[0].min.text == "0" && d.stats.columns[0].max.text == "39" ? "" : "wrong";
    }},
    {"a plan with no statistics is still a valid plan", [] {
        Database db(DIR, 32);
        std::string text = explainText(db, "SELECT x.k FROM NoStats x JOIN Emp e ON x.k = e.eid WHERE x.v = 'a';", 32);
        return text.find("seqscan NoStats") != std::string::npos ? "" : text;
    }},
    {"turning a rule off changes the plan the way it should", [] {
        Database db(DIR, 32);
        std::string noPush = explainText(db, QUERIES[2], 32, {2}), noJoin = explainText(db, QUERIES[2], 32, {3}), noProject = explainText(db, QUERIES[2], 32, {6});
        bool ok = noPush.find("filter (d.budget > 1000 and e.age > 60)") != std::string::npos && noJoin.find("filter (e.did = d.did)") != std::string::npos &&
                  noJoin.find("nested_loop_join") != std::string::npos && noProject.find("project [e.name, e.did]") == std::string::npos;
        return ok ? "" : noPush + noJoin + noProject;
    }},
    {"a predicate over three relations, or a whole WHERE with rule 1 off, joins at the first join that has them all", [] {
        Database db(DIR, 32);
        std::string three = explainText(db, "SELECT COUNT(*) FROM Dept d, Proj p, Loc l WHERE d.did = p.did OR p.pid = l.pid;", 32);
        std::string whole = explainText(db, QUERIES[10], 32, {1});
        bool ok = three.find("nested_loop_join (d.did = p.did or p.pid = l.pid)") != std::string::npos &&
                  whole.find("nested_loop_join (e.did = d.did and d.did = p.did and p.pid = l.pid and e.age > 66)") != std::string::npos;
        return ok ? "" : three + whole;
    }},
    {"rule 5 moves a select below a group_by only when there are grouping columns", [] {
        Database db(DIR, 32);
        auto plan = [&](const std::string& keys) {
            return logicalFromJson(parseJson("{\"op\":\"select\",\"condition\":\"1 = 2\",\"child\":{\"op\":\"group_by\",\"keys\":[" + keys + "],\"aggregates\":[[\"count\",\"*\",\"n\"]],\"child\":{\"op\":\"scan\",\"table\":\"Emp\"}}}"));
        };
        Optimizer o{db, 32, {}, {}};
        std::string keyed = o.optimize(plan("\"Emp.did\"")).op, none = o.optimize(plan("")).op;
        size_t rows = runPlan(db, plan(""), 32, true).size(), unoptimized = runPlan(db, plan(""), 32, false).size();
        return rows == 0 && unoptimized == 0 && keyed == "group_by" && none == "select" ? "" : std::to_string(rows) + " rows, unoptimized " + std::to_string(unoptimized) + ", at the top " + keyed + " and " + none;
    }},
    {"a sort below an operator that holds memory is costed with the B - 1 frames it gets", [] {
        Database db(DIR, 4);
        std::string problems;
        for (const std::string& sql : {"SELECT k, pad FROM Wide ORDER BY k;", "(SELECT k, pad FROM Wide ORDER BY k) EXCEPT (SELECT k, pad FROM Wide WHERE k < 0);"}) {
            Optimizer o{db, 4, {}, {}};
            LogicalNode plan = o.optimize(compileSql(db, sql));
            std::unique_ptr<Operator> root = buildPlan(db, physicalJson(plan));
            execute(db, *root);
            const LogicalNode* sort = &plan;
            while (sort->op != "sort") sort = &sort->kids[0];
            const Operator* sorter = root.get();
            while (sorter->op != "sort") sorter = sorter->children[0].get();
            unsigned long long actual = db.table("Wide").dataPages + sorter->counters.io.reads + sorter->counters.io.writes;
            if (std::llround(sort->cost) != (long long)actual) problems += sql + " estimated " + std::to_string(std::llround(sort->cost)) + ", read and sorted in " + std::to_string(actual) + " " + sorter->notes + "\n";
        }
        return problems;
    }},
    {"the optimizer gives SUM of a REAL the type REAL, and SUM of an INT or a BIGINT the type BIGINT", [] {
        Database db(DIR, 32);
        Optimizer o{db, 32, {}, {}};
        std::vector<Col> cols = o.outputCols(compileSql(db, "SELECT SUM(t.a), SUM(t.s), SUM(t.did) FROM (SELECT did, AVG(age) AS a, SUM(age) AS s FROM Emp GROUP BY did) AS t;"));
        return cols[0].type == RealT && cols[1].type == BigT && cols[2].type == BigT ? "" : "the types are " + typeName(cols[0]) + ", " + typeName(cols[1]) + " and " + typeName(cols[2]);
    }},
    {"conditions that rules 3 and 7 bring together past the height of one condition are split, and the query runs", [] {
        Database db(DIR, 32);
        std::string equalities = "e.did = d.did" + repeated(" AND e.did = d.did", 999), above, below;
        for (int i = 0; i < 1000; i++) { above += (i ? " AND did > -" : "did > -") + std::to_string(i + 1); below += (i ? " AND did < " : "did < ") + std::to_string(i + 5); }
        std::vector<Row> joined = runSql(db, "SELECT COUNT(*) FROM Emp e JOIN Dept d ON " + equalities + " WHERE " + equalities + ";", 32, true);
        std::vector<Row> grouped = runSql(db, "SELECT did, COUNT(*) FROM Emp WHERE " + above + " GROUP BY did HAVING " + below + ";", 32, true);
        bool ok = sameBag(joined, runSql(db, "SELECT COUNT(*) FROM Emp e JOIN Dept d ON e.did = d.did;", 32, true)) &&
                  sameBag(grouped, runSql(db, "SELECT did, COUNT(*) FROM Emp GROUP BY did HAVING did < 5;", 32, true)) && grouped.size() == 5;
        return ok ? "" : "wrong rows";
    }},
    {"the plans of statements at the limits of the SQL front end go through opt and exec", [] {
        std::string unions = "SELECT did FROM Dept WHERE did = 1" + repeated(" UNION ALL SELECT did FROM Dept WHERE did = 1", 1000) + ";";
        std::string nested = "SELECT eid FROM Emp WHERE eid < 5";
        for (int i = 0; i < 450; i++) { std::string t = "t" + std::to_string(i); nested = "SELECT " + t + ".eid FROM (" + nested + ") AS " + t + " WHERE " + t + ".eid < 5"; }
        std::string conjuncts = "SELECT COUNT(*) FROM Emp e, Dept d WHERE e.did = d.did";
        for (int i = 1; i < 1000; i++) conjuncts += " AND e.eid > -" + std::to_string(i);
        std::string got = chained(unions, {}) + ", " + chained(nested + ";", {}) + ", " + chained(conjuncts + ";", {"--without-rules", "7"});
        return got == "1001 rows, 5 rows, 1 rows" ? "" : got;
    }},
    {"explain writes a node a line, its fields separated by single spaces, then the total", [] {
        Database db(DIR, 32);
        std::string got = explainText(db, "SELECT name FROM Emp WHERE age > 60;", 32);
        return got == "project [Emp.name] est_rows=550 est_cost=0\n  filter (Emp.age > 60) est_rows=550 est_cost=0\n    seqscan Emp est_rows=3000 est_cost=30\ntotal est_cost=30\n" ? "" : got;
    }},
    {"ANALYZE needs a frame per column and one for the scan", [] {
        std::string got;
        for (int frames : {7, 8}) {
            Database db(DIR, frames);
            try { analyzeTable(db, db.table("Emp"), 20); got += "analyzed at " + std::to_string(frames) + "\n"; } catch (const Error& e) { got += e.msg + "\n"; }
        }
        return got == "error: ANALYZE of Emp needs at least 8 frames: one per column and one for the scan while it reads the table, and three for each column's sort after that\nanalyzed at 8\n" ? "" : got;
    }},
    {"opt tool commands", [] {
        std::ofstream(DIR + "/q.json") << dumpJson(logicalJson(compileSql(*openDatabase(DIR, 8), QUERIES[1])));
        std::ostringstream plan, explain, dp, bad;
        optTool({"plan", "--db", DIR, "--logical", DIR + "/q.json", "--frames", "16", "--json"}, plan);
        optTool({"explain", "--db", DIR, "--logical", DIR + "/q.json", "--frames", "16"}, explain);
        optTool({"dp-table", "--db", DIR, "--logical", DIR + "/q.json"}, dp);
        std::ofstream(DIR + "/bad.json") << "{\"op\":\"select\",\"child\":{\"op\":\"scan\",\"table\":\"Emp\"}}";
        optTool({"plan", "--db", DIR, "--logical", DIR + "/bad.json"}, bad);
        bool ok = plan.str().find("\"est_rows\"") != std::string::npos && explain.str().find("est_cost=") != std::string::npos && dp.str().find("{e,d}") != std::string::npos &&
                  bad.str() == "Node 'select' (root): missing required field 'condition'.\n";
        return ok ? "" : plan.str() + explain.str() + dp.str() + bad.str();
    }},
    {"the shell gives error positions in the whole input, as sql check does, from standard input and --file", [] {
        std::string dir = shellDatabase(), file = dir + "/q.sql";
        std::string text = "SELECT a FROM T;\nSELECT b FROM T;\nSELECT nope\n  FROM T;\n", explain = "  EXPLAIN  ANALYZE SELECT x FROM T;\n";
        std::ofstream(file) << text;
        std::ostringstream check;
        sqlTool({"check", "--db", dir, "--file", file}, check);
        std::ofstream(file) << text + explain;
        std::string want = check.str() + "error: name: unknown column x at line 5, column 27\nexit 1";
        std::string fromInput = shellErrors(text + explain, {"--db", dir}), fromFile = shellErrors("", {"--db", dir, "--file", file});
        std::filesystem::remove_all(dir);
        return check.str() == "error: name: unknown column nope at line 3, column 8\n" && fromInput == want && fromFile == want ? "" : check.str() + fromInput + "\n" + fromFile;
    }},
    {"the shell reports an unterminated string at the end of the input, and exits 1", [] {
        std::string dir = shellDatabase(), file = dir + "/q.sql";
        std::string text = "SELECT 'abc FROM T;\nSELECT COUNT(*) FROM T;\n";
        std::ofstream(file) << text;
        std::string want = "error: lexical: unterminated string at line 1, column 8\nexit 1";
        std::string fromInput = shellErrors(text, {"--db", dir}), fromFile = shellErrors("", {"--db", dir, "--file", file});
        std::filesystem::remove_all(dir);
        return fromInput == want && fromFile == want ? "" : fromInput + "\n" + fromFile;
    }},
    {"the shell takes a comment before EXPLAIN, ANALYZE, SET and LOAD", [] {
        std::string dir = shellDatabase(), csv = dir + "/t.csv";
        std::ofstream(csv) << "3,z\n";
        std::string got = shellErrors("-- 1\nEXPLAIN SELECT a FROM T;\n-- 2\nANALYZE T;\n-- 3\nSET frames = 8;\n-- 4\nLOAD T FROM '" + csv + "';\n", {"--db", dir});
        long long rows = Database(dir, 8).table("T").liveTuples;
        std::filesystem::remove_all(dir);
        return got == "exit 0" && rows == 3 ? "" : got;
    }},
    {"the shell refuses an ANALYZE bucket count that is not 1 to 999999999, and goes on", [] {
        std::string dir = shellDatabase();
        std::string got = shellErrors("ANALYZE T abc;\nANALYZE T 99999999999;\nANALYZE T 0;\nANALYZE T 2;\n", {"--db", dir});
        size_t buckets = Database(dir, 8).table("T").stats.columns[0].histogram.size();
        std::filesystem::remove_all(dir);
        std::string usage = "error: write ANALYZE table [buckets], with buckets from 1 to 999999999\n";
        return got == usage + usage + "error: the number of buckets must be at least 1\nexit 1" && buckets == 2 ? "" : got;
    }},
    {"SET frames that cannot reopen the database is an error, and the shell goes on with the database it had", [] {
        std::string dir = shellDatabase(), catalog = dir + "/catalog.db";
        std::ostringstream out;
        Shell shell(dir, 8, out);
        std::filesystem::rename(catalog, dir + "/catalog.kept");
        std::filesystem::create_directory(catalog);
        bool set = shell.runStatement("SET frames = 16;"), select = shell.runStatement("SELECT COUNT(*) FROM T;");
        std::filesystem::remove_all(dir);
        std::string text = out.str(), first = text.substr(0, text.find('\n') + 1), table = " count_star\n------------\n          2\n(1 row)";
        bool ok = !set && select && shell.frames == 8 && first.find("error: ") != std::string::npos && first.find(catalog) != std::string::npos &&
                  text.compare(first.size(), table.size(), table) == 0;
        return ok ? "" : text;
    }},
    {"an exception that is not an Error is one internal error line in opt, sql, the shell and each statement of the shell", [] {
        std::string dir = shellDatabase();
        std::ostringstream statementOut;
        Shell shell(dir, 8, statementOut);
        corruptStatistics(dir);
        bool set = shell.runStatement("SET frames = 16;"), select = shell.runStatement("SELECT COUNT(*) FROM T;");
        std::string statement = statementOut.str(), firstLine = statement.substr(0, statement.find('\n') + 1);
        std::ofstream(dir + "/q.sql") << "SELECT a FROM T;";
        std::ostringstream opt, sql;
        int optCode = optTool({"plan", "--db", dir, "--logical", dir + "/q.sql"}, opt), sqlCode = sqlTool({"check", "--db", dir, "--file", dir + "/q.sql"}, sql);
        std::string shellRun = shellErrors("", {"--db", dir});
        std::filesystem::remove_all(dir);
        bool ok = !set && select && oneInternalError(firstLine) && statement.find("(1 row)") != std::string::npos && optCode == 1 && oneInternalError(opt.str()) &&
                  sqlCode == 1 && oneInternalError(sql.str()) && shellRun.compare(0, 16, "internal error: ") == 0 && shellRun.substr(shellRun.size() - 7) == "\nexit 1";
        return ok ? "" : statement + opt.str() + sql.str() + shellRun;
    }},
    {"the shell refuses the table names the catalog refuses in the specification's error format", [] {
        std::string dir = shellDatabase();
        std::string got = shellErrors("CREATE TABLE catalog (x INT);\n  CREATE TABLE t (x INT);\nCREATE TABLE U (x INT);\n", {"--db", dir});
        size_t tables = Database(dir, 8).tables.size();
        std::filesystem::remove_all(dir);
        std::string want = "error: name: table name catalog is taken by the catalog's own file, catalog.db at line 1, column 14\n"
                           "error: name: table t would share a file with table T: table names may not differ only in case at line 2, column 16\nexit 1";
        return got == want && tables == 2 ? "" : got;
    }},
    {"the shell checks the frames before it opens the database, and a database it cannot open is an error, not an abort", [] {
        std::string dir = shellDatabase(), missing = DIR + "-none";
        std::string few = shellErrors("", {"--db", missing, "--frames", "0"}), set = shellErrors("SET frames = 3;\n", {"--db", dir}), file = shellErrors("", {"--db", dir + "/T.db"});
        bool created = std::filesystem::exists(missing);
        std::filesystem::remove_all(dir);
        size_t end = file.find('\n');
        bool oneError = end != std::string::npos && file.find("error: ") < end && file.substr(end) == "\nexit 1";
        bool ok = few == "error: --frames must be at least 4\nexit 1" && !created && set == "error: frames must be at least 4\nexit 1" && oneError;
        return ok ? "" : few + "\n" + set + "\n" + file;
    }},

    // ---- the cost model against what Module 2 does ----
    {"hash distinct and group_by cost what Module 2's hash grouping reads and writes", [] {
        std::string dir = DIR + "-hash", problems;
        std::filesystem::remove_all(dir);
        {
            Database db(dir, 64);
            std::vector<std::string> rows;
            for (int i = 0; i < 3000; i++) rows.push_back(std::to_string(i % 1000) + ",n" + std::to_string(i % 7) + "," + std::to_string(i));
            table(db, "G", "g:INT,name:VARCHAR(255),i:INT", rows);
            analyzeTable(db, db.table("G"), 20);
            // at these B no level is on the edge of fitting, where uneven partitions make some split and others not
            for (const std::string& sql : {"SELECT g, MAX(name) FROM G GROUP BY g;", "SELECT DISTINCT g, i FROM G;"})
                for (int b : {6, 8, 16}) {
                    Optimizer o{db, b, {}, {}};
                    LogicalNode plan = o.optimize(compileSql(db, sql));
                    LogicalNode* grouping = &plan;
                    while (grouping->op != "group_by" && grouping->op != "distinct") grouping = &grouping->kids[0];
                    grouping->algorithm = "hash";
                    o.estimate(plan, b);
                    Database run(dir, b);
                    std::unique_ptr<Operator> root = buildPlan(run, physicalJson(plan));
                    execute(run, *root);
                    const Operator* op = root.get();
                    while (op->op != grouping->op) op = op->children[0].get();
                    Counters actual = totals(*op);
                    double io = double(actual.io.reads + actual.io.writes);
                    if (std::fabs(grouping->cost - io) > 0.1 * io) problems += "B=" + std::to_string(b) + " " + sql + " estimated " + std::to_string(std::llround(grouping->cost)) + ", did " + std::to_string((long long)io) + " " + op->notes + "\n";
                }
        }
        std::filesystem::remove_all(dir);
        return problems;
    }},
    {"a distinct below a group_by with no keys is costed with all B frames, as Module 2 gives it", [] {
        Database db(DIR, 64);
        std::string problems;
        for (int b : {4, 5, 8}) {
            Optimizer o{db, b, {}, {}};
            LogicalNode below = o.optimize(compileSql(db, "SELECT COUNT(*) FROM (SELECT DISTINCT name, city FROM Emp) AS t;")), top = o.optimize(compileSql(db, "SELECT DISTINCT name, city FROM Emp;"));
            const LogicalNode* distinct = &below;
            while (distinct->op != "distinct") distinct = &distinct->kids[0];
            if (distinct->cost != top.cost) problems += "B=" + std::to_string(b) + " below " + std::to_string(distinct->cost) + ", at the top " + std::to_string(top.cost) + "\n";
        }
        return problems;
    }},
    {"the join enumeration costs a join below another with the B - 1 frames Module 2 gives it", [] {
        std::string dir = DIR + "-lower", problems;
        std::filesystem::remove_all(dir);
        {
            Database db(dir, 64);
            // 30 rows of 404 bytes are 3 pages, as the estimate says, ten to a page with their slots
            for (const char* name : {"A", "B", "C"}) {
                std::vector<std::string> rows;
                for (int i = 0; i < 30; i++) rows.push_back(std::to_string(i) + "," + std::string(198, 'p') + "," + std::string(198, 'q'));
                table(db, name, "k:INT,p:VARCHAR(200),q:VARCHAR(200)", rows);
                analyzeTable(db, db.table(name), 20);
            }
            int b = 5;
            Optimizer o{db, b, {}, {}};
            LogicalNode plan = o.optimize(compileSql(db, "SELECT * FROM A a, B b, C c WHERE a.k < b.k AND b.k < c.k;"));
            std::string outer, cost;
            for (const std::string& line : o.dpTable)
                if (line.rfind("{a,b,c}", 0) == 0 && line.find("* ") != std::string::npos) outer = line.substr(line.find("* ") + 2, line.find(" outer") - line.find("* ") - 2);
            for (const std::string& line : o.dpTable)
                if (!outer.empty() && line.rfind(outer + " ", 0) == 0 && line.find("* ") != std::string::npos) cost = line.substr(line.find("cost=") + 5);
            Database run(dir, b);
            std::unique_ptr<Operator> root = buildPlan(run, physicalJson(plan));
            execute(run, *root);
            const Operator* lower = root.get();
            while (lower->op != "nested_loop_join" || lower->children[0]->op == "nested_loop_join") lower = lower->children[0].get();
            Counters actual = totals(*lower);
            long long blocks = std::stoll(lower->notes.substr(lower->notes.find("blocks=") + 7));
            // the directory pages: the outer's once, the inner's once a block
            long long io = (long long)(actual.io.reads + actual.io.writes) - 1 - blocks;
            if (cost.empty() || std::stoll(cost) != io) problems += outer + " cost " + cost + ", read " + std::to_string(io) + " data pages " + lower->notes + "\n";
        }
        std::filesystem::remove_all(dir);
        return problems;
    }},
};

int runTests() {
    setup();
    int fails = 0;
    for (const Test& t : TESTS) {
        std::string problem;
        try { problem = t.run(); } catch (const Error& e) { problem = "threw " + e.msg; } catch (const SqlError& e) { problem = "threw " + sqlErrorText(e); }
        if (problem.empty()) continue;
        fails++;
        std::cout << "FAIL " << t.name << "\n" << problem << "\n";
    }
    std::filesystem::remove_all(DIR);
    std::cout << TESTS.size() - fails << "/" << TESTS.size() << " passed\n";
    return fails != 0;
}
int main() { return onLargeStack(std::cout, runTests); }
