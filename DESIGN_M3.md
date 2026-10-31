# DESIGN_M3.md: the SQL front end

Code: `src/sqlparse.hpp` (scanner, syntax tree, parser, tree printer), `src/sqlplan.hpp` (scopes, name
resolution, the type and aggregate rules, translation), `src/logical.hpp` (the logical plan and its
JSON), `src/sql.hpp` (the `sql` tool). Tests: `tests/test_sql.cpp`, with the seven examples of section 5,
all fifteen required cases and the error rules. The grammar is in `GRAMMAR_SQL.md`.

## 1 Bag semantics: where duplicate elimination happens

This is the decision the rest depends on, so it comes first. A logical plan from this module removes
duplicates in exactly four places, and nowhere else:

| SQL | plan | why there |
|---|---|---|
| `SELECT DISTINCT` | a `distinct` node directly above the select list's `project` | DISTINCT is defined on the rows the select list produces, so it must see the projected columns; below the projection it would compare columns the user did not ask for and keep rows that are duplicates in the output |
| `UNION` | a `union` node | set semantics by definition; `UNION ALL` is a `union_all` node, which keeps everything |
| `INTERSECT`, `EXCEPT` | `intersect`, `except` nodes | set semantics by definition, so the result has no duplicates even when the inputs do |
| `GROUP BY` | the `group_by` node | one row per group, which removes duplicate key combinations by construction |

Everything else keeps duplicates. A `scan` returns a bag; a `project` never removes duplicates, even when
it drops columns; joins and crosses multiply bags; `select` filters a bag. This is the change from Bonus
Project 1, where `project` was a set operation and deduplicated as a side effect. Case 23 of Bonus 1
changed answer for that reason (see DESIGN_LOG.md, session 8), and `distinct(e)` is now its own operator
in the algebra as well.

Two consequences I had to decide:

- **DISTINCT is not free.** The executor implements `distinct` by sorting or hashing the whole input,
  which for a large result is an external sort with its passes and writes. That is why the translator
  never adds a `distinct` the SQL did not ask for. Also, `SELECT DISTINCT a FROM R` and `SELECT a FROM R`
  are different plans that cost different amounts, even when `a` happens to be a key.
- **DISTINCT above GROUP BY.** The module document says DISTINCT over a GROUP BY is redundant. That is
  true only when every grouping column is in the select list, because then each output row is a
  different group. `SELECT DISTINCT did FROM Emp GROUP BY did, city` groups on two columns and shows
  one, so two groups can produce the same `did`, and the DISTINCT is needed. The translator drops the
  `distinct` only when every grouping column appears in the select list (tests `DISTINCT over a GROUP BY
  that shows every group column is not emitted` and `... that drops a group column is kept`).

## 2 Scope and name resolution

After parsing, the FROM clause is translated left to right, and each table reference adds one scope
entry: its alias if it has one, otherwise its table name. Every column of the entry is stored with that
name as its relation, so `FROM Emp e` makes columns `e.eid`, `e.name`, and so on. A subquery in FROM is
compiled on its own first, recursively with its own scope, and contributes its output columns under its
alias. That is how `t.n` in example 5.6 refers to the subquery's `COUNT(*) AS n`.

The resolution rules, in the order they are checked (`SqlCompiler::resolveColumn`):

1. A name that is already in scope when a new entry arrives is an error: `FROM Emp, Emp` gives
   `name: the name Emp appears twice in FROM, give one of them an alias` (case 12). `FROM Emp e, Emp m` is
   fine (case 1).
2. `t.c`: `t` must name an entry (`unknown table or alias x in x.name`), and `c` must be one of its columns
   (`column salaryy does not exist in e`). The two messages name which part failed.
3. `c` alone: exactly one entry may have it. None is `unknown column c`, and two or more is
   `ambiguity: column did is ambiguous, it could be Emp.did or Dept.did`, naming every candidate (case 3).
4. `SELECT *` expands to every column of every entry, in FROM order and then schema order, each written
   with its entry name. So two tables sharing a column name are not an error there (case 2).
5. `ORDER BY c`, with `c` alone, first looks for an output column named `c`: an alias, an aggregate's
   output name, or the name of a column selected without an alias. Two output columns named `c` from
   different sources are `ambiguity: ORDER BY c is ambiguous` (`SELECT name AS age, age ... ORDER BY
   age`, as in PostgreSQL); the same column twice is not. Otherwise, and always for `t.c`, it resolves
   against the FROM scope. A key that the select list shows under an alias sorts by the alias (`SELECT c
   AS x ... ORDER BY c` sorts by `x`). A column the select list does not have is allowed (case 9, below),
   except with `SELECT DISTINCT`, where it would change which rows are distinct (PostgreSQL refuses it
   too).
6. `GROUP BY c`, with `c` alone, is a FROM column when one has that name and otherwise the column that
   a select list alias `c` names (`SELECT did AS dd ... GROUP BY dd`), as in PostgreSQL. An alias of an
   aggregate is an aggregate error there. A column grouped twice is one key.
7. An unknown table is `name: unknown table Nope` at the table's position, a different message from an
   unknown column (case 11).

A `JOIN ... ON` condition is resolved against the tables of its own chain only, the ones to its left in
the chain and the one being joined: JOIN binds tighter than the comma, so in `FROM Dept d, Emp e JOIN
Works w ON ...` the condition cannot use `d`, which is PostgreSQL's rule too. After this pass every
column in the plan is written qualified (`Emp.age`, `e.did`). The only unqualified names in a plan are
aliases and the output names of aggregates (`n`, `count_star`), which belong to no table. The rule that
Module 2 and the optimizer resolve a name alone by (`resolve` in `algebra.hpp`) is: the column of no
relation with that name when there is one, and otherwise the one qualified column with that name. So
`SELECT d.did AS budget FROM Dept d ORDER BY d.budget` works although its projection holds both
`budget` and `d.budget`.

**Output names.** An output column has the name of its alias, its aggregate or its column. A later
column with the qualified name of an earlier one gets `_2`, `_3`, ...: `SELECT name, name` gives
`Emp.name` and `name_2`, `SELECT COUNT(*), COUNT(*)` computes the count once and gives `count_star` and
`count_star_2`. A subquery in FROM gets its alias on every column, so `(SELECT t.pid, w.pid ...) AS s`
would have `s.pid` twice; its later columns are renamed the same way (`s.pid`, `s.pid_2`) by a
`project` under the `rename`. PostgreSQL keeps both names and refuses `s.pid` as ambiguous; a schema
here cannot hold one qualified name twice, so the second column needs a name of its own.

## 3 Type rules

Comparisons need the same type on both sides, equality included, as in Bonus Project 1. INT, and the
numeric results of aggregates, compare with each other. VARCHAR compares only with VARCHAR. Case 10 is
`type: cannot compare age (INT) with '30' (VARCHAR(2))`. `SUM` and `AVG` need a number: an INT column, or
the BIGINT of a SUM or the REAL of an AVG computed in a subquery. `SUM` gives a BIGINT, or a REAL over a
REAL, and `AVG` a REAL. `COUNT` takes anything, `MIN` and `MAX` take either type. The set operators need the same number of columns and
comparable types in each position (case 14), and the output takes the left input's names. `INSERT`
checks the number of values, each value's type against its column, that an INT fits in 32 bits and that
a string fits its VARCHAR. Every integer literal is an INT, so one outside -2147483648 to 2147483647 is a
type error that says so, also in a comparison (`WHERE a < 3000000000`), where PostgreSQL would make it
a BIGINT.

## 4 Aggregation rules

A query block aggregates when it has GROUP BY, or an aggregate in its select list or ORDER BY. Then the
select list may hold only grouping columns and aggregates (case 6), as may HAVING and ORDER BY. An
aggregate in WHERE is always an error, because WHERE runs before grouping (case 7). One `group_by` node
computes every aggregate the block uses: the ones in the select list, then any that only HAVING or
ORDER BY mention, which are carried as extra output columns and projected away. An aggregate without an
alias that is identical to one already computed (`COUNT(*)` in the select list and in HAVING) is that
one, computed once.

**Aggregate naming rule.** The output column of an aggregate is named by its `AS` alias when it has one
(`COUNT(*) AS n` gives `n`). Otherwise the name is the function, an underscore, and the qualified
column with its dot replaced by an underscore: `count_star` for `COUNT(*)`, `avg_Emp_salary` for
`AVG(salary)` over `Emp`, and `sum_e_salary` for `SUM(e.salary)`. This is the rule the Module 2
document's group_by example uses, so a plan from this module and one written by hand name things the
same way. HAVING refers to aggregates by these names: `HAVING COUNT(*) > 5` becomes `select n > 5`
above the group_by when the select list called it `n`. A default name that an alias of the select list
or another aggregate already has, and an alias that another aggregate already has, gets `_2`, `_3`, ...:
in `SELECT did AS count_star, COUNT(*) ... GROUP BY did` the count is `count_star_2`, so ORDER BY
`count_star` still means the alias.

## 5 Translation

The translator follows the canonical order of section 5 of the module, and makes no optimisation
decisions:

1. FROM: each table reference is a `scan`, with a `rename` above it when it has an alias. Each
   `JOIN ... ON` chain becomes `join` nodes, left deep, carrying their ON condition. The comma separated
   chains are combined by `cross` nodes, left deep. A subquery is its own plan under a `rename`, with a
   `project` between them only when two of its columns share a name (section 2).
2. WHERE: one `select` with the whole condition, unchanged. The predicate stays above the joins even
   when it touches one table; moving it is Module 4's job (example 5.2).
3. GROUP BY: one `group_by` with the grouping columns and every aggregate, even when there is no GROUP BY
   but the select list aggregates. `COUNT(*)` alone gives `keys []` and one row out of an empty table
   (case 13).
4. HAVING: a `select` above the group_by (case 5, example 5.4).
5. SELECT: a `project`, then `distinct` when DISTINCT was written and can change anything (section 1).
6. ORDER BY: a `sort`. When a key is not in the select list (case 9), the projection keeps it as an
   extra column, the sort uses it, and a second `project` above the sort drops it again:
   `project [Emp.name] / sort [Emp.age desc] / project [Emp.name, Emp.age] / scan Emp`.
7. LIMIT: a `limit`.
8. The set operators between whole terms, left associative. `ORDER BY` and `LIMIT` after the last term
   then apply to the result of the set operations (GRAMMAR_SQL.md section 4).

Conditions are written into the plan as text in Bonus Project 1's condition language (`and`, `or`,
`not`, `!=`). The text is printed from a Bonus 1 condition tree, built from the SQL condition after
resolution, by the same `condText` that the plan JSON round trip uses. So parentheses appear exactly
where the grammar needs them, and Module 2 parses the text back with `parseCondition`. The SQL tree holds
a chain of ORs or ANDs as one node with all its operands; the Bonus 1 tree is binary and left deep, and
`mk` counts such a chain as one level of nesting, so a 1000 term OR is as acceptable as a short one.

## 6 Errors

Everything is reported as `error: <category>: <message> at line L, column C` with one of the seven
categories (lexical, syntax, name, ambiguity, type, aggregate, schema), at the first error, with no stack
trace. `sql check` prints nothing and exits 0 when the file is correct, and prints the error and exits 1
otherwise. A statement past the limits of GRAMMAR_SQL.md section 5 (450 levels of nesting, 1000
comparisons in a condition, 1000 set operators) is a syntax error in the same form, never a crash. In a script, `CREATE TABLE` adds the table to what later statements see, without touching the
database, so a file that creates a table and then queries it compiles.

## 7 Sources and what they changed

- Ramakrishnan and Gehrke, chapters 4 and 5. Their translation of a basic SQL block into
  selection-projection-cross product, with the WHERE clause as one selection over the product, is the
  canonical order this translator emits. They describe SQL's default as multiset semantics, which made
  me remove the deduplication from Bonus 1's projection rather than add a flag to it.
- Nystrom, *Crafting Interpreters*, chapters 6 to 8. Its precedence ladder, one function per level
  each calling the next, is how both the condition grammar and the set operator level are written. Its
  advice to report the first error with a position and stop (panic mode without recovery) is the
  error strategy here.
- The PostgreSQL documentation on SELECT. It settled the questions the module left open: ORDER BY and
  LIMIT applying to a whole UNION, ORDER BY a column not in the select list (allowed, but not with
  DISTINCT), and INNER JOIN as a spelling of JOIN. The workload is checked against PostgreSQL, so where
  it was silent the choice was to match it.
