// Module 4: the worked examples, the checklist, and SQL run through all four modules
#include "../src/opt.hpp"
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
std::vector<Row> runSql(Database& db, const std::string& sql, int frames, bool optimized, std::string* physical = nullptr) {
    Optimizer o{db, frames, {}, {}};
    LogicalNode logical = compileSql(db, sql);
    LogicalNode plan = optimized ? o.optimize(logical) : o.baseline(logical);
    if (!optimized) o.estimate(plan);
    Json json = physicalJson(plan);
    if (physical) *physical = dumpJson(json);
    Database run(DIR, frames);
    std::unique_ptr<Operator> root = buildPlan(run, parseJson(dumpJson(json)));
    return execute(run, *root).rows;
}
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
        std::string small = explainText(db, QUERIES.back(), 4), large = explainText(db, QUERIES.back(), 64);
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
};

int main() {
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
