# GRAMMAR_SQL.md

The SQL subset of Module 3, in the same notation as `GRAMMAR.md` (ISO/IEC 14977 EBNF: `=` defines,
`,` concatenates, `|` separates alternatives, `[ ]` is optional, `{ }` repeats zero or more times,
terminals are quoted). The parser in `src/sqlparse.hpp` has one method per nonterminal below, and
`sql parse --query "..." --explain` prints the tree it builds, so every grouping can be checked.

## 1 Lexical grammar

```
letter     = "A" | ... | "Z" | "a" | ... | "z" ;
digit      = "0" | ... | "9" ;
IDENT      = ( letter | "_" ) , { letter | digit | "_" } - KEYWORD ;   (* case sensitive *)
KEYWORD    = any spelling, in any case, of one of the words in the table below ;
INTEGER    = [ "-" | "+" ] , digit , { digit } ;                       (* not followed by a letter *)
STRING     = "'" , { character - ( "'" | newline ) | "''" } , "'" ;    (* '' is one quote *)
OPERATOR   = "=" | "<>" | "!=" | "<" | "<=" | ">" | ">=" ;               (* maximal munch, != means <> *)
PUNCT      = "(" | ")" | "," | "." | ";" | "*" ;
comment    = "--" , { character - newline } ;
```

Keywords (case insensitive, reserved): `SELECT DISTINCT FROM WHERE GROUP BY HAVING ORDER ASC DESC LIMIT
UNION ALL INTERSECT EXCEPT JOIN INNER ON AS AND OR NOT CREATE TABLE INSERT INTO VALUES INT VARCHAR COUNT
SUM AVG MIN MAX`.

The scanner is hand written (`sqlTokens`): no regular expressions. Maximal munch decides `<=`, `>=`,
`<>` and `!=` by looking at one more character. A `-` or `+` is part of a number only when a digit
follows (integers may be signed, as the module says; the `+` is dropped, so `+1` is `1`); `--` starts a
comment; any other `-` or `+` is a lexical error, because the subset has no arithmetic. A number
running straight into a letter (`12ab`) is a lexical error rather than two tokens. Every token keeps its
line and column, and every error reports the position of the token that caused it.

## 2 Grammar

```
script       = { statement , ";" } , [ statement ] ;
statement    = create | insert | query ;

create       = "CREATE" , "TABLE" , IDENT , "(" , coldef , { "," , coldef } , ")" ;
coldef       = IDENT , ( "INT" | "VARCHAR" , "(" , INTEGER , ")" ) ;          (* 1 <= n <= 255 *)
insert       = "INSERT" , "INTO" , IDENT , "VALUES" , "(" , literal , { "," , literal } , ")" ;
literal      = INTEGER | STRING ;

query        = term , { setop , term } , [ "ORDER" , "BY" , sortitem , { "," , sortitem } ] , [ "LIMIT" , INTEGER ] ;
setop        = "UNION" , [ "ALL" ] | "INTERSECT" | "EXCEPT" ;
term         = select | "(" , query , ")" ;
select       = "SELECT" , [ "DISTINCT" ] , ( "*" | item , { "," , item } ) ,
               "FROM" , chain , { "," , chain } ,
               [ "WHERE" , condition ] ,
               [ "GROUP" , "BY" , column , { "," , column } , [ "HAVING" , condition ] ] ;
item         = ( column | aggregate ) , [ [ "AS" ] , IDENT ] ;
sortitem     = ( column | aggregate ) , [ "ASC" | "DESC" ] ;
aggregate    = ( "COUNT" | "SUM" | "AVG" | "MIN" | "MAX" ) , "(" , ( column | "*" ) , ")" ;   (* * only for COUNT *)
chain        = tableref , { [ "INNER" ] , "JOIN" , tableref , "ON" , condition } ;
tableref     = IDENT , [ [ "AS" ] , IDENT ] | "(" , query , ")" , [ "AS" ] , IDENT ;
column       = IDENT , [ "." , IDENT ] ;

condition    = conjunction , { "OR" , conjunction } ;
conjunction  = negation , { "AND" , negation } ;
negation     = "NOT" , negation | "(" , condition , ")" | comparison ;
comparison   = operand , OPERATOR , operand ;
operand      = column | INTEGER | STRING | aggregate ;      (* an aggregate is legal only in HAVING *)
```

The grammar is LL(1) except at one place: after `(` in `negation` the next token decides between a
parenthesised condition and nothing else, because an operand can never start with `(`. So one token of
lookahead is enough everywhere and the parser never backtracks.

A chain `c1 OR c2 OR ... OR cn` (and the same with `AND`) is one node of the syntax tree holding all n
operands, not n - 1 nested nodes, so a long chain makes the tree wide and not deep. Its meaning is the
left associative one of the table below.

## 3 Precedence and associativity

Precedence is in the grammar, not in parser code. Each level is its own nonterminal and calls only the
next tighter one, exactly as in Bonus Project 1.

| level | operators | associativity | nonterminal |
|---|---|---|---|
| 1 (tightest) | comparisons `=` `<>` `<` `<=` `>` `>=` | none, a comparison has two operands | comparison |
| 2 | `NOT` | right, prefix: `NOT NOT c` is `NOT (NOT c)` | negation |
| 3 | `AND` | left | conjunction |
| 4 | `OR` | left | condition |
| 5 | `JOIN ... ON` | left: `A JOIN B ON p JOIN C ON q` is `(A JOIN B) JOIN C` | chain |
| 6 | `,` in FROM | left: chains are combined by cross products left to right | select |
| 7 | `UNION`, `UNION ALL`, `INTERSECT`, `EXCEPT` | left, all at one level | query |

The repetition `term , { setop , term }` is the rewrite of the left recursive rule
`query = query , setop , term | term`, which is what makes the set operators left associative:
`A UNION B EXCEPT C` can only be derived as `(A UNION B) EXCEPT C` (tested). Parentheses are the
only way to get the other grouping, `A UNION (B EXCEPT C)`, which is also tested and gives a
different plan. The same holds for `AND` and `OR`; `a = 1 OR b = 2 AND c = 3` is
`a = 1 OR (b = 2 AND c = 3)`.

**Here the set operators differ from PostgreSQL.** The module puts all four at one level, and so does
this grammar. PostgreSQL, following the SQL standard, binds `INTERSECT` tighter than `UNION` and
`EXCEPT`. So `A UNION B INTERSECT C` is `(A UNION B) INTERSECT C` here (tested) and
`A UNION (B INTERSECT C)` in PostgreSQL, and the two can return different rows. A query that
parenthesises every mix of `INTERSECT` with `UNION` or `EXCEPT` means the same in both.

Because `JOIN` binds tighter than the comma, an `ON` condition belongs to its own chain and can use
only the tables of that chain, as in PostgreSQL. `FROM Dept d, Emp e JOIN Works w ON d.did = e.did` is
the name error `d is not part of this JOIN, so its ON condition cannot use d.did`; the condition
belongs in WHERE.

## 4 Where this grammar differs from the sketch in section 2 of the module, and why

1. **`ORDER BY` and `LIMIT` belong to the whole query, not to the last select block.** In the sketch
   they are part of `select_block`, so in `SELECT a FROM R UNION SELECT a FROM S ORDER BY a LIMIT 5`
   they would sort and cut only the second block. PostgreSQL, the SQL standard and the lectures (lecture
   06's set operation slides) apply them to the result of the union. The integration workload is checked
   against PostgreSQL, so I followed it. For a query with one block the plan is unchanged: sort and limit
   still sit above that block's projection, in the canonical order. A parenthesised term of a set
   operation can still have its own `ORDER BY` and `LIMIT`. A parenthesised query on its own, as in
   `(SELECT name FROM Emp WHERE eid < 3) ORDER BY eid`, is that query, as in PostgreSQL: the `ORDER BY`
   and `LIMIT` after it apply as if written inside the parentheses, so they may name a column of its
   FROM clause, and a second `ORDER BY` or `LIMIT` inside is the syntax error `multiple ORDER BY clauses
   are not allowed`. The parser folds such a query into the one inside. After a set operation, ORDER BY
   names a column of the result by its name alone: a table name (`ORDER BY e.name`), an aggregate or a
   column the result does not have is an error, as in PostgreSQL.
2. **`AS` is optional** before an alias, in the select list and in FROM. The sketch writes `AS`, but the
   module's own examples write `FROM Emp e`, and the lectures write `Student s`, `avg(salary) avg` and
   a subquery alias without `AS`.
3. **`!=` is accepted and means `<>`.** Lecture 05's list of comparison operators gives both.
4. **`INNER JOIN` is accepted** as a spelling of `JOIN`, as in PostgreSQL. Outer joins are not.
5. **Commas and `JOIN` may be mixed.** The sketch allows either a comma list or one chain of joins.
   Here a FROM clause is a comma list of join chains, which includes both of the sketch's forms. The
   chains are combined by cross products left to right, which is what PostgreSQL means by it too.
6. **An aggregate may be an operand of a comparison and an `ORDER BY` key.** The sketch's `operand` has
   only columns and literals, but HAVING needs `COUNT(*) > 5` (example 5.4), and `ORDER BY COUNT(*) DESC`
   is common. Semantic analysis rejects an aggregate anywhere but HAVING, the select list and ORDER BY
   (case 7).
7. **`COUNT`, `SUM`, `AVG`, `MIN`, `MAX`, `INT` and `VARCHAR` are reserved.** The sketch writes them as
   terminals, so they are keywords, and identifiers cannot use them. A column called `avg` (used as an
   alias on one lecture slide) has to be renamed.

## 5 Limits

A statement may nest parentheses, `NOT` and subqueries (in FROM, or a parenthesised query) at most 450
levels deep, all three counted together; a condition (one WHERE, HAVING or ON) may hold at most 1000
comparisons; a statement may hold at most 1000 set operators; and a FROM clause may name at most 16
tables, a subquery counting as one (its own FROM clause may name 16 more). Past a limit the statement
is refused with a syntax error where the limit was crossed, for example `error: syntax: a condition may
hold at most 1000 comparisons at line 1, column 10922`. A chain of `AND`s or `OR`s is not nesting,
however long it is; only its comparisons count, towards the 1000.

The limits are there because several parts of the system walk these trees recursively, and each has to
stay inside the stack: the condition trees of Bonus 1 that conditions are written in (`mk` refuses one
nested deeper than 500 or taller than 2000 levels, and a condition of 1000 comparisons inside 450
levels of nesting is within both), the optimizer, which places every conjunct of a WHERE in its own
recursive walk down the plan, and the plan itself, whose depth grows with every set operator. Measured
in the shell (`make` build) on the 8 MB stack of a main thread with the limits lifted, a WHERE of 2000
comparisons joined by `AND` ran and one of 2500 overflowed the stack, and 2000 blocks joined by `UNION
ALL` ran and 3000 overflowed, so on that stack the limits leave a margin of about two; the tools now
work on a larger one (below). PostgreSQL has limits of the same kind: it refuses 20000 blocks joined by
`UNION ALL` with "stack depth limit exceeded".

The table limit is for the optimizer, which orders the tables of a FROM clause by trying every subset of
them (DESIGN_M4.md section 5). Sixteen tables with no join predicate between them take 0.8 seconds and
600 MB to plan, and every table more doubles both; at 32 the subsets no longer fit the 32 bits the
search keeps them in, and before this limit 32 tables crashed the optimizer and 33 gave a wrong count.
PostgreSQL stops reordering a FROM list at 8 tables (`from_collapse_limit`) and searches heuristically
from 12 (`geqo_threshold`).

**The deepest plan.** The plans that `sql compile` writes, and that `opt plan` writes for them, are
read back by `opt` and `exec`, whose JSON reader refuses a document nested deeper than `MAX_JSON_DEPTH`
so that a hand-written one cannot overflow the stack. That limit is computed from the limits above, in
one place (`sqlparse.hpp`), as the deepest plan a statement within them can become, plus three for the
arrays inside a node and the array `sql compile` puts around the plans of several statements: 45514. A
path from the root of a plan meets each set operator at most once, and at most 451 query blocks, each
nested in the one before. A block adds at most three levels per table of its FROM clause (a join, the
projection rule 6 puts on its input, and a filter split off a condition too tall to be one, DESIGN_M4.md
section 3) and 13 others: limit, the projection that drops sort keys the select list does not show,
sort, distinct, the select list's projection, the HAVING filter, group_by, the projection rule 6 puts
below it, the WHERE filter above the joins, a filter on a table, a filter moved in from the block
around it, a subquery's rename and the projection that renames its repeated columns. With rule 7 off
(`opt plan --without-rules 7`) every conjunct is a filter of its own, and room is left for the
conjuncts of one block's WHERE, HAVING and ON conditions, 17 × 1000; only a statement that nests
several such blocks inside each other makes a deeper plan, which `exec` then refuses with a JSON
syntax error. Measured: 1000 `UNION ALL`s, 450 nested FROM subqueries with a WHERE each, and a WHERE
of 999 comparisons through `opt plan --without-rules 7` run through the shell and through the three
tools chained by file, and so do 449 nested blocks of 16 tables each, whose plan is 16615 levels deep.

These depths take more stack than the 8 MB of a main thread, by far in a sanitizer build, where the
recursion takes up to 6 KB per level. So `sql`, `opt`, `exec` and `db` do their work on a thread with a
stack of 1 GB (`onLargeStack` in `cli.hpp`; `tests/test_opt.cpp` runs on one too). The time
is the other cost: the optimizer works out a node's columns again at every level above it, so its time
grows with the square of the plan's depth, and the 16615-level plan takes the shell four minutes. Its
JSON, which indents each level two spaces more, is 3.8 GB.

## 6 Keywords and identifiers

Keywords are case insensitive and come out of the scanner upper case. Identifiers are case sensitive,
as the module document requires: `Emp` and `emp` are different tables and `select Name from emp` is a
name error. This is a deliberate difference from PostgreSQL, which folds unquoted identifiers to
lower case.
