// Module 3: the translation examples of section 5, the fifteen required cases, and the error rules
#include "../src/sql.hpp"
#include <sstream>

const std::string DIR = "tests/tmp-sql";

std::string run(std::vector<std::string> args) { std::ostringstream out; sqlTool(args, out); return out.str(); }
std::string compile(const std::string& text, bool json = false) {
    std::ofstream(DIR + "/q.sql") << text;
    std::vector<std::string> args = {"compile", "--db", DIR, "--file", DIR + "/q.sql"};
    if (json) args.push_back("--json");
    return run(args);
}
int checkCode(const std::string& text) { std::ofstream(DIR + "/q.sql") << text; std::ostringstream out; return sqlTool({"check", "--db", DIR, "--file", DIR + "/q.sql"}, out); }

void setup() {
    std::filesystem::remove_all(DIR);
    Database db(DIR, 8);
    db.createTable("Emp", parseSchema("eid:INT,name:VARCHAR(20),age:INT,did:INT,salary:INT,mgr:INT,city:VARCHAR(20)"));
    db.createTable("Dept", parseSchema("did:INT,title:VARCHAR(20),budget:INT,city:VARCHAR(20)"));
    db.createTable("Proj", parseSchema("pid:INT,did:INT,name:VARCHAR(20)"));
    db.createTable("Loc", parseSchema("lid:INT,pid:INT,place:VARCHAR(20)"));
}

// n comparisons, name = 0 op name = 1 op ...
std::string chain(const std::string& name, const std::string& op, int n) {
    std::string s;
    for (int i = 0; i < n; i++) s += (i ? " " + op + " " : "") + name + " = " + std::to_string(i);
    return s;
}
std::string repeated(const std::string& s, int n) { std::string out; for (int i = 0; i < n; i++) out += s; return out; }
std::string nestedSubqueries(int n) {
    std::string q = "SELECT did FROM Emp";
    for (int i = 0; i < n; i++) q = "SELECT s" + std::to_string(i) + ".did FROM (" + q + ") AS s" + std::to_string(i);
    return q + ";";
}
// the column just after the text before it, ending an error line
std::string columnAfter(const std::string& before) { return std::to_string(before.size() + 1) + "\n"; }

struct Case { const char* name; std::string sql; std::string want; bool json = false; };
const std::vector<Case> CASES = {
    {"5.1 plain filter and projection", "SELECT name, age FROM Emp WHERE age > 30;",
     "{\n  \"op\": \"project\",\n  \"columns\": [\"Emp.name\", \"Emp.age\"],\n  \"child\": {\n    \"op\": \"select\",\n    \"condition\": \"Emp.age > 30\",\n    \"child\": {\n      \"op\": \"scan\",\n      \"table\": \"Emp\"\n    }\n  }\n}\n", true},
    {"5.2 join with aliases, the WHERE stays above the join", "SELECT e.name, d.title FROM Emp e JOIN Dept d ON e.did = d.did WHERE d.budget > 1000;",
     "project [e.name, d.title]\n  select d.budget > 1000\n    join e.did = d.did\n      rename e\n        scan Emp\n      rename d\n        scan Dept\n"},
    {"5.3 comma join is a cross under the select", "SELECT * FROM Emp e, Dept d WHERE e.did = d.did;",
     "project [e.eid, e.name, e.age, e.did, e.salary, e.mgr, e.city, d.did, d.title, d.budget, d.city]\n  select e.did = d.did\n    cross\n      rename e\n        scan Emp\n      rename d\n        scan Dept\n"},
    {"5.4 grouping with HAVING", "SELECT did, COUNT(*) AS n, AVG(salary) AS avg_sal FROM Emp WHERE age > 25 GROUP BY did HAVING COUNT(*) > 5 ORDER BY n DESC LIMIT 10;",
     "{\n  \"op\": \"limit\",\n  \"n\": 10,\n  \"child\": {\n    \"op\": \"sort\",\n    \"keys\": [\n      [\"n\", \"desc\"]\n    ],\n    \"child\": {\n      \"op\": \"project\",\n      \"columns\": [\"Emp.did\", \"n\", \"avg_sal\"],\n"
     "      \"child\": {\n        \"op\": \"select\",\n        \"condition\": \"n > 5\",\n        \"child\": {\n          \"op\": \"group_by\",\n          \"keys\": [\"Emp.did\"],\n          \"aggregates\": [\n            [\"count\", \"*\", \"n\"],\n            [\"avg\", \"Emp.salary\", \"avg_sal\"]\n          ],\n"
     "          \"child\": {\n            \"op\": \"select\",\n            \"condition\": \"Emp.age > 25\",\n            \"child\": {\n              \"op\": \"scan\",\n              \"table\": \"Emp\"\n            }\n          }\n        }\n      }\n    }\n  }\n}\n", true},
    {"5.5 and case 1: a self join is two renames over one table", "SELECT e.name, m.name FROM Emp e JOIN Emp m ON e.mgr = m.eid;",
     "project [e.name, m.name]\n  join e.mgr = m.eid\n    rename e\n      scan Emp\n    rename m\n      scan Emp\n"},
    {"5.6 subquery in FROM", "SELECT t.did, t.n FROM (SELECT did, COUNT(*) AS n FROM Emp GROUP BY did) AS t WHERE t.n > 3;",
     "project [t.did, t.n]\n  select t.n > 3\n    rename t\n      project [Emp.did, n]\n        group_by keys [Emp.did] aggregates [count(*) as n]\n          scan Emp\n"},
    {"5.7 and case 4: UNION ALL against UNION on the same inputs", "SELECT city FROM Emp UNION ALL SELECT city FROM Dept; SELECT city FROM Emp UNION SELECT city FROM Dept;",
     "union_all\n  project [Emp.city]\n    scan Emp\n  project [Dept.city]\n    scan Dept\nunion\n  project [Emp.city]\n    scan Emp\n  project [Dept.city]\n    scan Dept\n"},
    {"case 2: SELECT * over three tables, two sharing a column name", "SELECT * FROM Emp, Dept, Proj;",
     "project [Emp.eid, Emp.name, Emp.age, Emp.did, Emp.salary, Emp.mgr, Emp.city, Dept.did, Dept.title, Dept.budget, Dept.city, Proj.pid, Proj.did, Proj.name]\n  cross\n    cross\n      scan Emp\n      scan Dept\n    scan Proj\n"},
    {"case 3: an unqualified column in two tables", "SELECT did FROM Emp, Dept;", "error: ambiguity: column did is ambiguous, it could be Emp.did or Dept.did at line 1, column 8\n"},
    {"case 5: WHERE below the grouping, HAVING above it", "SELECT did, SUM(salary) FROM Emp WHERE city = 'Ottawa' GROUP BY did HAVING SUM(salary) > 100000;",
     "project [Emp.did, sum_Emp_salary]\n  select sum_Emp_salary > 100000\n    group_by keys [Emp.did] aggregates [sum(Emp.salary) as sum_Emp_salary]\n      select Emp.city = 'Ottawa'\n        scan Emp\n"},
    {"case 6: a column that is neither grouped nor aggregated", "SELECT name, did, COUNT(*) FROM Emp GROUP BY did;",
     "error: aggregate: column Emp.name must appear in GROUP BY or be used in an aggregate function at line 1, column 8\n"},
    {"case 7: an aggregate in WHERE", "SELECT did FROM Emp WHERE COUNT(*) > 2;", "error: aggregate: aggregate functions are not allowed in WHERE at line 1, column 27\n"},
    {"case 8: a subquery in FROM joined to a base table", "SELECT d.title, t.total FROM Dept d JOIN (SELECT did, SUM(salary) AS total FROM Emp GROUP BY did) t ON d.did = t.did;",
     "project [d.title, t.total]\n  join d.did = t.did\n    rename d\n      scan Dept\n    rename t\n      project [Emp.did, total]\n        group_by keys [Emp.did] aggregates [sum(Emp.salary) as total]\n          scan Emp\n"},
    {"case 9: ORDER BY a column the select list does not have", "SELECT name FROM Emp ORDER BY age DESC, name;",
     "project [Emp.name]\n  sort [Emp.age desc, Emp.name asc]\n    project [Emp.name, Emp.age]\n      scan Emp\n"},
    {"case 10: an INT compared with a string", "SELECT name FROM Emp WHERE age > '30';", "error: type: cannot compare age (INT) with '30' (VARCHAR(2)) at line 1, column 32\n"},
    {"case 11: unknown table", "SELECT x FROM Nope;", "error: name: unknown table Nope at line 1, column 15\n"},
    {"case 11: unknown column, a different message", "SELECT salaryy FROM Emp;", "error: name: unknown column salaryy at line 1, column 8\n"},
    {"case 11: unknown column of a known alias", "SELECT e.salaryy FROM Emp e;", "error: name: column salaryy does not exist in e at line 1, column 8\n"},
    {"case 11: unknown alias", "SELECT x.name FROM Emp e;", "error: name: unknown table or alias x in x.name at line 1, column 8\n"},
    {"case 12: the same table twice without aliases", "SELECT * FROM Emp, Emp;", "error: name: the name Emp appears twice in FROM, give one of them an alias at line 1, column 20\n"},
    {"case 13: COUNT(*) with no GROUP BY", "SELECT COUNT(*) FROM Emp;", "project [count_star]\n  group_by keys [] aggregates [count(*) as count_star]\n    scan Emp\n"},
    {"case 14: a UNION whose column types differ", "SELECT name FROM Emp UNION SELECT did FROM Dept;", "error: schema: column 1 of UNION is VARCHAR(20) on the left and INT on the right at line 1, column 28\n"},
    {"case 15: four tables with commas", "SELECT e.name, l.place FROM Emp e, Dept d, Proj p, Loc l WHERE e.did = d.did AND d.did = p.did AND p.pid = l.pid AND e.age > 30;",
     "project [e.name, l.place]\n  select e.did = d.did and d.did = p.did and p.pid = l.pid and e.age > 30\n    cross\n      cross\n        cross\n          rename e\n            scan Emp\n          rename d\n            scan Dept\n        rename p\n          scan Proj\n      rename l\n        scan Loc\n"},
    {"case 15: the same four tables with JOIN ON", "SELECT e.name, l.place FROM Emp e JOIN Dept d ON e.did = d.did JOIN Proj p ON d.did = p.did JOIN Loc l ON p.pid = l.pid WHERE e.age > 30;",
     "project [e.name, l.place]\n  select e.age > 30\n    join p.pid = l.pid\n      join d.did = p.did\n        join e.did = d.did\n          rename e\n            scan Emp\n          rename d\n            scan Dept\n        rename p\n          scan Proj\n      rename l\n        scan Loc\n"},

    // the rest of the language
    {"keywords in any case, identifiers exactly", "select Name from emp;", "error: name: unknown table emp at line 1, column 18\n"},
    {"keywords in any case", "sElEcT name FrOm Emp wHeRe age >= 3 oR NOT age <> 4 AnD age != 5;",
     "project [Emp.name]\n  select Emp.age >= 3 or not Emp.age != 4 and Emp.age != 5\n    scan Emp\n"},
    {"NOT binds tighter than AND, AND tighter than OR, parentheses kept where they matter", "SELECT name FROM Emp WHERE (age = 1 OR age = 2) AND NOT (did = 3 AND city = 'x');",
     "project [Emp.name]\n  select (Emp.age = 1 or Emp.age = 2) and not (Emp.did = 3 and Emp.city = 'x')\n    scan Emp\n"},
    {"set operators are left associative", "SELECT city FROM Emp UNION SELECT city FROM Dept EXCEPT SELECT name FROM Proj;",
     "except\n  union\n    project [Emp.city]\n      scan Emp\n    project [Dept.city]\n      scan Dept\n  project [Proj.name]\n    scan Proj\n"},
    {"parentheses change the grouping of set operators", "SELECT city FROM Emp UNION (SELECT city FROM Dept EXCEPT SELECT name FROM Proj);",
     "union\n  project [Emp.city]\n    scan Emp\n  except\n    project [Dept.city]\n      scan Dept\n    project [Proj.name]\n      scan Proj\n"},
    {"ORDER BY and LIMIT after a set operation apply to all of it", "SELECT city FROM Emp UNION SELECT city FROM Dept ORDER BY city DESC LIMIT 2;",
     "limit 2\n  sort [Emp.city desc]\n    union\n      project [Emp.city]\n        scan Emp\n      project [Dept.city]\n        scan Dept\n"},
    {"SELECT DISTINCT is a distinct above the projection", "SELECT DISTINCT city FROM Emp;", "distinct\n  project [Emp.city]\n    scan Emp\n"},
    {"DISTINCT over a GROUP BY that shows every group column is not emitted", "SELECT DISTINCT did, COUNT(*) FROM Emp GROUP BY did;",
     "project [Emp.did, count_star]\n  group_by keys [Emp.did] aggregates [count(*) as count_star]\n    scan Emp\n"},
    {"DISTINCT over a GROUP BY that drops a group column is kept", "SELECT DISTINCT did FROM Emp GROUP BY did, city;",
     "distinct\n  project [Emp.did]\n    group_by keys [Emp.did, Emp.city] aggregates []\n      scan Emp\n"},
    {"aliases without AS, ORDER BY an output alias, an aggregate only in ORDER BY", "SELECT did d, MAX(age) oldest FROM Emp GROUP BY did ORDER BY oldest DESC, MIN(age);",
     "project [d, oldest]\n  sort [oldest desc, min_Emp_age asc]\n    project [Emp.did as d, oldest, min_Emp_age]\n      group_by keys [Emp.did] aggregates [max(Emp.age) as oldest, min(Emp.age) as min_Emp_age]\n        scan Emp\n"},
    {"an aggregate only in HAVING", "SELECT did FROM Emp GROUP BY did HAVING AVG(age) > 30 AND did <> 2;",
     "project [Emp.did]\n  select avg_Emp_age > 30 and Emp.did != 2\n    group_by keys [Emp.did] aggregates [avg(Emp.age) as avg_Emp_age]\n      scan Emp\n"},
    {"HAVING on a column that is not grouped", "SELECT did FROM Emp GROUP BY did HAVING age > 3;", "error: aggregate: column Emp.age must appear in GROUP BY or be used in an aggregate function at line 1, column 41\n"},
    {"SUM of a VARCHAR", "SELECT SUM(name) FROM Emp;", "error: type: SUM needs a number, Emp.name is VARCHAR(20) at line 1, column 8\n"},
    {"comparing two columns of different types", "SELECT * FROM Emp e JOIN Dept d ON e.name = d.did;", "error: type: cannot compare e.name (VARCHAR(20)) with d.did (INT) at line 1, column 43\n"},
    {"a UNION with different numbers of columns", "SELECT city FROM Emp UNION SELECT city, did FROM Dept;", "error: schema: the inputs of UNION have 1 and 2 columns at line 1, column 28\n"},
    {"a subquery in FROM needs an alias", "SELECT * FROM (SELECT did FROM Emp);", "error: syntax: expected an alias, which a subquery in FROM must have but found ';' at line 1, column 36\n"},
    {"lexical errors", "SELECT name FROM Emp WHERE city = 'Ott;", "error: lexical: unterminated string at line 1, column 35\n"},
    {"lexical errors", "SELECT name FROM Emp WHERE age > 3 # 4;", "error: lexical: unexpected character '#' at line 1, column 36\n"},
    {"lexical errors", "SELECT name FROM Emp\nWHERE city = 'caf\xe9';", "error: lexical: the text is not valid UTF-8 at line 2, column 18\n"},
    {"syntax errors carry the position", "SELECT name\nFROM Emp\nWHERE age > ;", "error: syntax: expected a column, a number or a string but found ';' at line 3, column 13\n"},
    {"comments and a doubled quote", "-- every employee called O'Brien\nSELECT name FROM Emp WHERE name = 'O''Brien'; -- done",
     "project [Emp.name]\n  select Emp.name = 'O''Brien'\n    scan Emp\n"},
    {"CREATE and INSERT are checked, and later statements see the new table",
     "CREATE TABLE Pet (id INT, kind VARCHAR(5)); INSERT INTO Pet VALUES (1, 'cat'); SELECT kind FROM Pet;",
     "{\"statement\":\"create_table\",\"table\":\"Pet\",\"columns\":[[\"id\",\"INT\"],[\"kind\",\"VARCHAR\",5]]}\n{\"statement\":\"insert\",\"table\":\"Pet\",\"values\":[1,\"cat\"]}\nproject [Pet.kind]\n  scan Pet\n"},
    {"INSERT with a value of the wrong type", "INSERT INTO Dept VALUES (1, 'a', 'b', 'c');", "error: type: column budget is INT but the value 'b' is a string at line 1, column 34\n"},
    {"INSERT with a string too long", "CREATE TABLE P (k VARCHAR(2)); INSERT INTO P VALUES ('abc');", "error: type: 'abc' is longer than VARCHAR(2) at line 1, column 54\n"},
    {"CREATE of a table that exists", "CREATE TABLE Emp (a INT);", "error: name: table Emp already exists at line 1, column 1\n"},

    // long and deep statements
    {"a chain of 1000 ORs is one level of nesting", "SELECT COUNT(*) FROM Emp WHERE " + chain("eid", "OR", 1000) + ";",
     "project [count_star]\n  group_by keys [] aggregates [count(*) as count_star]\n    select " + chain("Emp.eid", "or", 1000) + "\n      scan Emp\n"},
    {"a chain of 1000 ANDs is one level of nesting", "SELECT eid FROM Emp WHERE " + chain("eid", "AND", 1000) + ";",
     "project [Emp.eid]\n  select " + chain("Emp.eid", "and", 1000) + "\n    scan Emp\n"},
    {"8000 ORs are an error, not a crash", "SELECT COUNT(*) FROM Emp WHERE " + chain("eid", "OR", 8000) + ";",
     "error: syntax: a condition may hold at most 1000 comparisons at line 1, column " + columnAfter("SELECT COUNT(*) FROM Emp WHERE " + chain("eid", "OR", 1000) + " OR eid ")},
    {"each condition has its own count of comparisons", "SELECT did FROM Emp WHERE " + chain("eid", "OR", 1000) + " GROUP BY did HAVING " + chain("did", "OR", 1000) + ";",
     "project [Emp.did]\n  select " + chain("Emp.did", "or", 1000) + "\n    group_by keys [Emp.did] aggregates []\n      select " + chain("Emp.eid", "or", 1000) + "\n        scan Emp\n"},
    {"449 NOTs and a parenthesis around 1000 comparisons", "SELECT eid FROM Emp WHERE " + repeated("NOT ", 449) + "(" + chain("eid", "OR", 1000) + ");",
     "project [Emp.eid]\n  select " + repeated("not ", 449) + "(" + chain("Emp.eid", "or", 1000) + ")\n    scan Emp\n"},
    {"451 NOTs are too deep", "SELECT eid FROM Emp WHERE " + repeated("NOT ", 451) + "eid = 1;",
     "error: syntax: nesting too deep, at most 450 levels of parentheses, NOT and subqueries at line 1, column " + columnAfter("SELECT eid FROM Emp WHERE " + repeated("NOT ", 451))},
    {"450 parentheses around a condition", "SELECT eid FROM Emp WHERE " + repeated("(", 450) + "eid = 1" + repeated(")", 450) + ";", "project [Emp.eid]\n  select Emp.eid = 1\n    scan Emp\n"},
    {"450 parentheses around a query", repeated("(", 450) + "SELECT eid FROM Emp" + repeated(")", 450) + ";", "project [Emp.eid]\n  scan Emp\n"},
    {"451 nested subqueries are too deep", nestedSubqueries(451),
     "error: syntax: nesting too deep, at most 450 levels of parentheses, NOT and subqueries at line 1, column " + columnAfter(nestedSubqueries(451).substr(0, nestedSubqueries(451).find("SELECT did FROM Emp")))},
    {"1001 set operators are an error, not a crash", "SELECT did FROM Emp" + repeated(" UNION ALL SELECT did FROM Emp", 1001) + ";",
     "error: syntax: a statement may hold at most 1000 set operators at line 1, column " + columnAfter("SELECT did FROM Emp" + repeated(" UNION ALL SELECT did FROM Emp", 1000) + " ")},

    // names
    {"an alias that another column has as its name, and ORDER BY that column", "SELECT d.did AS budget FROM Dept d ORDER BY d.budget;",
     "project [budget]\n  sort [d.budget asc]\n    project [d.did as budget, d.budget]\n      rename d\n        scan Dept\n"},
    {"ORDER BY the source of an aliased column sorts by the alias", "SELECT e.name AS title FROM Emp e JOIN Dept d ON e.did = d.did ORDER BY d.title, e.name;",
     "project [title]\n  sort [d.title asc, title asc]\n    project [e.name as title, d.title]\n      join e.did = d.did\n        rename e\n          scan Emp\n        rename d\n          scan Dept\n"},
    {"an aggregate named like a group column, used in HAVING", "SELECT did, COUNT(*) AS did FROM Emp GROUP BY did HAVING COUNT(*) > 100;",
     "project [Emp.did, did]\n  select did > 100\n    group_by keys [Emp.did] aggregates [count(*) as did]\n      scan Emp\n"},
    {"ORDER BY a name two output columns have", "SELECT name AS age, age FROM Emp ORDER BY age;",
     "error: ambiguity: ORDER BY age is ambiguous, two columns of the select list are named age at line 1, column 43\n"},
    {"ORDER BY an output column name before the FROM scope", "SELECT e.did AS x, d.did FROM Emp e, Dept d ORDER BY did;",
     "sort [d.did asc]\n  project [e.did as x, d.did]\n    cross\n      rename e\n        scan Emp\n      rename d\n        scan Dept\n"},
    {"a column selected twice is renamed the second time", "SELECT name, name FROM Emp ORDER BY name;",
     "sort [Emp.name asc]\n  project [Emp.name, Emp.name as name_2]\n    scan Emp\n"},
    {"an aggregate selected twice is computed once", "SELECT COUNT(*), COUNT(*) FROM Emp;",
     "project [count_star, count_star as count_star_2]\n  group_by keys [] aggregates [count(*) as count_star]\n    scan Emp\n"},
    {"an aggregate again without its alias", "SELECT MAX(age) AS m, MAX(age) FROM Emp;", "project [m, m as m_2]\n  group_by keys [] aggregates [max(Emp.age) as m]\n    scan Emp\n"},
    {"a default aggregate name an alias has", "SELECT did AS count_star, COUNT(*) FROM Emp GROUP BY did ORDER BY count_star;",
     "sort [count_star asc]\n  project [Emp.did as count_star, count_star_2]\n    group_by keys [Emp.did] aggregates [count(*) as count_star_2]\n      scan Emp\n"},
    {"two aliases alike are ambiguous in ORDER BY", "SELECT name AS x, city AS x FROM Emp ORDER BY x;",
     "error: ambiguity: ORDER BY x is ambiguous, two columns of the select list are named x at line 1, column 47\n"},
    {"a subquery with two columns of one name", "SELECT * FROM (SELECT e.did, d.did FROM Emp e, Dept d) AS s;",
     "project [s.did, s.did_2]\n  rename s\n    project [e.did, d.did as did_2]\n      project [e.did, d.did]\n        cross\n          rename e\n            scan Emp\n          rename d\n            scan Dept\n"},
    {"DISTINCT with ORDER BY the source of an aliased column", "SELECT DISTINCT city AS c FROM Emp ORDER BY city;",
     "sort [c asc]\n  distinct\n    project [Emp.city as c]\n      scan Emp\n"},
    {"an ON condition sees only its own JOIN chain", "SELECT * FROM Dept d, Emp e JOIN Proj p ON d.did = p.did;",
     "error: name: d is not part of this JOIN, so its ON condition cannot use d.did at line 1, column 44\n"},
    {"a column of an earlier comma item is unknown in ON", "SELECT * FROM Dept d, Emp e JOIN Proj p ON budget = p.did;", "error: name: unknown column budget at line 1, column 44\n"},
    {"ORDER BY after a set operation takes no table name", "SELECT e.name FROM Emp e UNION SELECT d.title FROM Dept d ORDER BY e.name;",
     "error: name: ORDER BY after a set operation can only name the result's columns at line 1, column 68\n"},

    // grouping and types
    {"GROUP BY an output alias", "SELECT did AS dd, COUNT(*) FROM Emp GROUP BY dd;", "project [Emp.did as dd, count_star]\n  group_by keys [Emp.did] aggregates [count(*) as count_star]\n    scan Emp\n"},
    {"GROUP BY a FROM column before an alias", "SELECT age AS did, COUNT(*) FROM Emp GROUP BY did;",
     "error: aggregate: column Emp.age must appear in GROUP BY or be used in an aggregate function at line 1, column 8\n"},
    {"GROUP BY the alias of an aggregate", "SELECT COUNT(*) AS n FROM Emp GROUP BY n;",
     "error: aggregate: GROUP BY n names COUNT(*), and aggregate functions are not allowed in GROUP BY at line 1, column 40\n"},
    {"a column grouped twice is one key", "SELECT did, COUNT(*) FROM Emp GROUP BY did, did;", "project [Emp.did, count_star]\n  group_by keys [Emp.did] aggregates [count(*) as count_star]\n    scan Emp\n"},
    {"SUM and AVG of a SUM and an AVG", "SELECT SUM(t.s), AVG(t.s), SUM(t.a), AVG(t.a) FROM (SELECT did, SUM(age) AS s, AVG(age) AS a FROM Emp GROUP BY did) AS t;",
     "project [sum_t_s, avg_t_s, sum_t_a, avg_t_a]\n  group_by keys [] aggregates [sum(t.s) as sum_t_s, avg(t.s) as avg_t_s, sum(t.a) as sum_t_a, avg(t.a) as avg_t_a]\n    rename t\n"
     "      project [Emp.did, s, a]\n        group_by keys [Emp.did] aggregates [sum(Emp.age) as s, avg(Emp.age) as a]\n          scan Emp\n"},
    {"an integer may have a plus sign", "SELECT name FROM Emp WHERE age > +1; INSERT INTO Proj VALUES (+2, -3, 'x');",
     "project [Emp.name]\n  select Emp.age > 1\n    scan Emp\n{\"statement\":\"insert\",\"table\":\"Proj\",\"values\":[2,-3,\"x\"]}\n"},
    {"an integer too big for an INT says why", "SELECT name FROM Emp WHERE age < 3000000000;",
     "error: type: 3000000000 does not fit in an INT, which has 4 bytes and holds -2147483648 to 2147483647 at line 1, column 34\n"},

    // LIMIT inside a set operation and a subquery
    {"LIMIT in a parenthesised set operand", "SELECT title FROM Dept UNION ALL (SELECT name FROM Emp ORDER BY name LIMIT 2);",
     "union_all\n  project [Dept.title]\n    scan Dept\n  limit 2\n    sort [Emp.name asc]\n      project [Emp.name]\n        scan Emp\n"},
    {"LIMIT in a subquery in FROM", "SELECT s.did, d.title FROM (SELECT did FROM Emp ORDER BY did DESC LIMIT 2) AS s JOIN Dept d ON s.did = d.did;",
     "project [s.did, d.title]\n  join s.did = d.did\n    rename s\n      limit 2\n        sort [Emp.did desc]\n          project [Emp.did]\n            scan Emp\n    rename d\n      scan Dept\n"},
};

int main() {
    setup();
    int fails = 0, total = 0;
    for (const Case& c : CASES) {
        total++;
        std::string got = compile(c.sql, c.json);
        if (got == c.want) continue;
        fails++;
        std::cout << "FAIL " << c.name << "\n" << c.sql << "\n--- want ---\n" << c.want << "--- got ---\n" << got;
    }
    // check prints nothing and exits 0 when there is no error, and exits 1 with the error otherwise
    total++;
    if (checkCode("SELECT name FROM Emp;") != 0 || checkCode("SELECT nope FROM Emp;") != 1) { fails++; std::cout << "FAIL check exit codes\n"; }
    total++;
    std::string tree = run({"parse", "--query", "SELECT DISTINCT e.name AS n FROM Emp e JOIN (SELECT did FROM Dept) AS d ON e.did = d.did WHERE NOT e.age > 3 GROUP BY e.name ORDER BY n LIMIT 2", "--explain"});
    std::string wantTree = "query\n  select distinct\n    column e.name as n\n  from\n    table Emp as e\n    join on e.did = d.did\n      subquery as d\n        query\n          select\n            column did\n          from\n            table Dept\n"
                           "  where NOT e.age > 3\n  group by e.name\n  order by n asc\n  limit 2\n";
    if (tree != wantTree) { fails++; std::cout << "FAIL parse --explain\n" << tree; }
    std::filesystem::remove_all(DIR);
    std::cout << total - fails << "/" << total << " passed\n";
    return fails != 0;
}
