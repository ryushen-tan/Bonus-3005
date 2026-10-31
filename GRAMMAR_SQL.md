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
INTEGER    = [ "-" ] , digit , { digit } ;                             (* not followed by a letter *)
STRING     = "'" , { character - ( "'" | newline ) | "''" } , "'" ;    (* '' is one quote *)
OPERATOR   = "=" | "<>" | "!=" | "<" | "<=" | ">" | ">=" ;               (* maximal munch, != means <> *)
PUNCT      = "(" | ")" | "," | "." | ";" | "*" ;
comment    = "--" , { character - newline } ;
```

Keywords (case insensitive, reserved): `SELECT DISTINCT FROM WHERE GROUP BY HAVING ORDER ASC DESC LIMIT
UNION ALL INTERSECT EXCEPT JOIN INNER ON AS AND OR NOT CREATE TABLE INSERT INTO VALUES INT VARCHAR COUNT
SUM AVG MIN MAX`.

The scanner is hand written (`sqlTokens`): no regular expressions. Maximal munch decides `<=`, `>=`,
`<>` and `!=` by looking at one more character. A `-` is part of a number only when a digit follows;
`--` starts a comment; any other `-` is a lexical error, because the subset has no arithmetic. A number
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

## 4 Where this grammar differs from the sketch in section 2 of the module, and why

1. **`ORDER BY` and `LIMIT` belong to the whole query, not to the last select block.** In the sketch
   they are part of `select_block`, so in `SELECT a FROM R UNION SELECT a FROM S ORDER BY a LIMIT 5`
   they would sort and cut only the second block. PostgreSQL, the SQL standard and the lectures (lecture
   06's set operation slides) apply them to the result of the union. The integration workload is checked
   against PostgreSQL, so I followed it. For a query with one block the plan is unchanged: sort and limit
   still sit above that block's projection, in the canonical order. A parenthesised term can still have
   its own `ORDER BY` and `LIMIT`.
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

## 5 Keywords and identifiers

Keywords are case insensitive and come out of the scanner upper case. Identifiers are case sensitive,
as the module document requires: `Emp` and `emp` are different tables and `select Name from emp` is a
name error. This is a deliberate difference from PostgreSQL, which folds unquoted identifiers to
lower case.
