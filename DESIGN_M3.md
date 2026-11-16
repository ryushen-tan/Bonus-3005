# DESIGN_M3.md: the SQL front end

The code is in `src/sqlparse.hpp` (scanner, syntax tree, parser, tree printer), `src/sqlplan.hpp` (scopes, name
resolution, the type and aggregate rules, translation), `src/logical.hpp` (the logical plan and its
JSON) and `src/sql.hpp` (the `sql` tool). The tests are in `tests/test_sql.cpp`. They cover the seven examples of section 5,
all fifteen required cases and the error rules. The grammar is in `GRAMMAR_SQL.md`.

## 1 Bag semantics: where duplicate elimination happens

I'm starting with this because the rest depends on it. A bag is a table that can hold the same row more than once. A logical plan from this module removes duplicates in exactly four places and nowhere else.

- `SELECT DISTINCT` becomes a `distinct` node directly above the `project` of the select list. DISTINCT is about the rows the select list produces, so it has to see the projected columns. Below the projection it would compare columns the user did not ask for, and it would keep rows that are duplicates in the output.
- `UNION` becomes a `union` node. Sets have no duplicates by definition. `UNION ALL` becomes a `union_all` node, which keeps everything.
- `INTERSECT` and `EXCEPT` become `intersect` and `except` nodes. These are also set operations, so the result has no duplicates even when the inputs do.
- `GROUP BY` becomes the `group_by` node. It gives one row per group, so duplicate key combinations are gone automatically.

Everything else keeps duplicates. A `scan` returns a bag. A `project` never removes duplicates, even when it drops columns. Joins and crosses multiply bags, and `select` just filters a bag. This is a change from Bonus Project 1, where `project` was a set operation and removed duplicates as a side effect. Case 23 of Bonus 1 changed its answer because of this (see DESIGN_LOG.md, session 8). `distinct(e)` is now its own operator in the algebra too.

I had to decide two things because of this.

First, DISTINCT is not free. The executor does `distinct` by sorting or hashing the whole input. For a large result that means an external sort with its passes and writes. So the translator never adds a `distinct` the SQL did not ask for. It also means `SELECT DISTINCT a FROM R` and `SELECT a FROM R` are different plans with different costs, even when `a` happens to be a key.

Second, DISTINCT on top of GROUP BY. The module document says DISTINCT over a GROUP BY is redundant. That is only true when every grouping column is in the select list, because then each output row is a different group. `SELECT DISTINCT did FROM Emp GROUP BY did, city` groups on two columns but shows one. Two groups can give the same `did`, so the DISTINCT is needed. The translator drops the `distinct` only when every grouping column appears in the select list. Two tests check this: `DISTINCT over a GROUP BY that shows every group column is not emitted` and `... that drops a group column is kept`.

## 2 Scope and name resolution

Name resolution means working out which table and column each name in the query refers to. After parsing, the FROM clause is translated left to right. Each table reference adds one scope entry, named by its alias if it has one and by its table name if not. Every column of the entry is stored with that name as its relation, so `FROM Emp e` makes columns `e.eid`, `e.name` and so on. A subquery in FROM is compiled on its own first, with its own scope, and its output columns go in under its alias. That is how `t.n` in example 5.6 refers to the subquery's `COUNT(*) AS n`.

These are the resolution rules, in the order they are checked (`SqlCompiler::resolveColumn`).

1. If a name is already in scope when a new entry arrives, that is an error. `FROM Emp, Emp` gives `name: the name Emp appears twice in FROM, give one of them an alias` (case 12). `FROM Emp e, Emp m` is fine (case 1).
2. For `t.c`, `t` must be an entry (`unknown table or alias x in x.name`) and `c` must be one of its columns (`column salaryy does not exist in e`). The two messages say which part failed.
3. For `c` alone, exactly one entry may have that column. If none does, the error is `unknown column c`. If two or more do, it is `ambiguity: column did is ambiguous, it could be Emp.did or Dept.did`, which names every candidate (case 3).
4. `SELECT *` expands to every column of every entry, in FROM order and then schema order, each written with its entry name. So two tables that share a column name are not an error there (case 2).
5. `ORDER BY c`, with `c` alone, first looks for an output column named `c`. That can be an alias, an aggregate's output name, or the name of a column selected without an alias. If two output columns named `c` come from different sources, it is `ambiguity: ORDER BY c is ambiguous` (`SELECT name AS age, age ... ORDER BY age`, as in PostgreSQL). The same column twice is not an error. If there is no output column with that name, and always for `t.c`, the name is resolved against the FROM scope. If the select list shows a key under an alias, the sort uses the alias (`SELECT c AS x ... ORDER BY c` sorts by `x`). A column the select list does not have is allowed (case 9, below), except with `SELECT DISTINCT`, where it would change which rows are distinct. PostgreSQL refuses that too.
6. `GROUP BY c`, with `c` alone, is a FROM column if one has that name. Otherwise it is the column that a select list alias `c` names (`SELECT did AS dd ... GROUP BY dd`), as in PostgreSQL. An alias of an aggregate is an aggregate error there. A column grouped twice is one key.
7. An unknown table is `name: unknown table Nope` at the table's position. That is a different message from an unknown column (case 11).

A `JOIN ... ON` condition is resolved only against the tables of its own chain. Those are the ones to its left in the chain and the one being joined. JOIN binds tighter than the comma, so in `FROM Dept d, Emp e JOIN Works w ON ...` the condition cannot use `d`. PostgreSQL has the same rule.

After this pass, every column in the plan is written qualified (`Emp.age`, `e.did`). The only unqualified names in a plan are aliases and the output names of aggregates (`n`, `count_star`), because they belong to no table. Module 2 and the optimizer look up a name given alone with `resolve` in `algebra.hpp`. It picks the column of no relation with that name if there is one. If not, it picks the one qualified column with that name. So `SELECT d.did AS budget FROM Dept d ORDER BY d.budget` works, even though its projection holds both `budget` and `d.budget`.

Output names work like this. An output column is named by its alias, its aggregate or its column. If a later column has the same qualified name as an earlier one, it gets `_2`, `_3` and so on. `SELECT name, name` gives `Emp.name` and `name_2`. `SELECT COUNT(*), COUNT(*)` computes the count once and gives `count_star` and `count_star_2`. A subquery in FROM gets its alias on every column, so `(SELECT t.pid, w.pid ...) AS s` would have `s.pid` twice. Its later columns are renamed the same way (`s.pid`, `s.pid_2`) by a `project` under the `rename`. PostgreSQL keeps both names and refuses `s.pid` as ambiguous. A schema here can't hold one qualified name twice, so the second column needs a name of its own.

## 3 Type rules

Both sides of a comparison need the same type, and that includes equality, as in Bonus Project 1. INT and the numeric results of aggregates can be compared with each other. VARCHAR can only be compared with VARCHAR. Case 10 gives `type: cannot compare age (INT) with '30' (VARCHAR(2))`.

`SUM` and `AVG` need a number. That means an INT column, or the BIGINT of a SUM or the REAL of an AVG computed in a subquery. `SUM` gives a BIGINT, or a REAL over a REAL, and `AVG` gives a REAL. `COUNT` takes anything, and `MIN` and `MAX` take either type.

The set operators need the same number of columns and comparable types in each position (case 14). The output takes the left input's names.

`INSERT` checks the number of values, the type of each value against its column, that an INT fits in 32 bits, and that a string fits its VARCHAR. Every integer literal is an INT. So a literal outside -2147483648 to 2147483647 is a type error that says so. This also holds in a comparison (`WHERE a < 3000000000`), where PostgreSQL would make it a BIGINT. A literal is judged by its value, the same way the loader judges a field, so `000000000001` is the INT 1.

## 4 Aggregation rules

A query block aggregates when it has GROUP BY, or an aggregate in its select list or ORDER BY. In that case the select list may hold only grouping columns and aggregates (case 6), and so may HAVING and ORDER BY. An aggregate in WHERE is always an error, because WHERE runs before grouping (case 7).

One `group_by` node computes every aggregate the block uses. First come the ones in the select list. Then come any that only HAVING or ORDER BY mention. Those are carried as extra output columns and projected away later. If an aggregate has no alias and is identical to one already computed (`COUNT(*)` in the select list and in HAVING), it is that one, computed once.

The aggregate naming rule: the output column of an aggregate is named by its `AS` alias if it has one (`COUNT(*) AS n` gives `n`). If not, the name is the function, an underscore, and the qualified column with its dot replaced by an underscore. That gives `count_star` for `COUNT(*)`, `avg_Emp_salary` for `AVG(salary)` over `Emp`, and `sum_e_salary` for `SUM(e.salary)`. This is the rule the Module 2 document's group_by example uses, so a plan from this module and one written by hand name things the same way. HAVING refers to aggregates by these names. `HAVING COUNT(*) > 5` becomes `select n > 5` above the group_by when the select list called it `n`.

Sometimes a default name is already taken by an alias in the select list or by another aggregate. The same goes for an alias that another aggregate already has. In those cases the new one gets `_2`, `_3` and so on. In `SELECT did AS count_star, COUNT(*) ... GROUP BY did` the count is `count_star_2`, so ORDER BY `count_star` still means the alias.

## 5 Translation

The translator follows the canonical order of section 5 of the module. It makes no optimisation decisions.

1. FROM: each table reference is a `scan`, with a `rename` above it when it has an alias. Each `JOIN ... ON` chain becomes `join` nodes, left deep, carrying their ON condition. The comma separated chains are combined by `cross` nodes, left deep. A subquery is its own plan under a `rename`. A `project` goes between them only when two of its columns share a name (section 2).
2. WHERE: one `select` with the whole condition, unchanged. The predicate stays above the joins even when it only touches one table. Moving it is Module 4's job (example 5.2).
3. GROUP BY: one `group_by` with the grouping columns and every aggregate. This happens even when there is no GROUP BY but the select list aggregates. `COUNT(*)` alone gives `keys []` and one row out of an empty table (case 13).
4. HAVING: a `select` above the group_by (case 5, example 5.4).
5. SELECT: a `project`, then `distinct` when DISTINCT was written and can change anything (section 1).
6. ORDER BY: a `sort`. When a key is not in the select list (case 9), the projection keeps it as an extra column and the sort uses it. A second `project` above the sort then drops it again: `project [Emp.name] / sort [Emp.age desc] / project [Emp.name, Emp.age] / scan Emp`.
7. LIMIT: a `limit`.
8. The set operators go between whole terms, left associative. `ORDER BY` and `LIMIT` after the last term then apply to the result of the set operations (GRAMMAR_SQL.md section 4). A parenthesised query on its own is not a set operation. The parser folds `(SELECT ...) ORDER BY ... LIMIT ...` into the query inside, so its ORDER BY and LIMIT are steps 6 and 7 of that block, as in PostgreSQL.

Conditions are written into the plan as text in Bonus Project 1's condition language (`and`, `or`, `not`, `!=`). The text is printed from a Bonus 1 condition tree. That tree is built from the SQL condition after resolution, and it is printed by the same `condText` that the plan JSON round trip uses. So parentheses appear exactly where the grammar needs them, and Module 2 parses the text back with `parseCondition`. The SQL tree holds a chain of ORs or ANDs as one node with all its operands. The Bonus 1 tree is binary and left deep, and `mk` counts such a chain as one level of nesting. So a 1000 term OR is as acceptable as a short one.

## 6 Errors

Every error is reported as `error: <category>: <message> at line L, column C`. There are seven categories: lexical, syntax, name, ambiguity, type, aggregate and schema. Only the first error is reported, and there is no stack trace. `sql check` prints nothing and exits 0 when the file is correct. Otherwise it prints the error and exits 1.

A statement past the limits of GRAMMAR_SQL.md section 5 (450 levels of nesting, 1000 comparisons in a condition, 1000 set operators, 16 tables in a FROM clause) is a syntax error in the same form. It never crashes. A statement within the limits becomes a plan, and the optimizer makes another plan from it. Both are accepted by `opt` and `exec`. Their JSON reader's depth limit is computed from these limits (GRAMMAR_SQL.md section 5, which also has the one exception, a nest of blocks with rule 7 off).

In a script, `CREATE TABLE` adds the table to what later statements see, without touching the database. So a file that creates a table and then queries it compiles. A `CREATE TABLE` is also checked by the catalog's own rules for the name of a new table (`tableNameProblem` in `catalog.hpp`). A table is a file, so its name is at most 252 characters, never `catalog` in any case, and never an existing table's name in another case. So `sql check`, `sql compile` and the shell all refuse `CREATE TABLE dept (x INT)` beside `Dept`, as a name error at the table's name.

## 7 Sources and what they changed

Ramakrishnan and Gehrke, chapters 4 and 5. They translate a basic SQL block into selection, projection and cross product, with the WHERE clause as one selection over the product. That is the canonical order this translator uses. They also describe SQL's default as multiset semantics. That made me remove the deduplication from Bonus 1's projection instead of adding a flag to it.

Nystrom, *Crafting Interpreters*, chapters 6 to 8. Its precedence ladder has one function per level, each calling the next. I wrote both the condition grammar and the set operator level this way. Its advice to report the first error with a position and stop (panic mode without recovery) is the error strategy here.

The PostgreSQL documentation on SELECT. It settled the questions the module left open: ORDER BY and LIMIT applying to a whole UNION, ORDER BY a column not in the select list (allowed, but not with DISTINCT), and INNER JOIN as a spelling of JOIN. The workload is checked against PostgreSQL, so where the module was silent I chose to match it.
