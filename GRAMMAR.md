# GRAMMAR.md

The grammar in sections 1 and 2, with precedence and associativity decided, was written down before
the parser; this document was assembled from it and checked against the program afterwards. The
parser in `src/parser.hpp` has one method per nonterminal below (the three keyword alternatives of Primary share a `unary` method, and CmpOp is a
table inside `cmp`), so every grouping decision can be checked against `ra --tree`.

## 1 The grammar

### 1.1 What language it generates

The grammar generates the set of relational algebra queries over named relations: relation names
combined with the three unary operators `select`, `project`, `rename` (prefix, with a parameter in
square brackets and the input in parentheses) and the five binary operators `union`, `intersect`,
`minus`, `times`, `join` (infix), where a condition is a boolean combination of comparisons between
attributes and constants. A second start symbol generates the relation definition files that supply
the data. Both are context-free grammars. After the stratification in section 2 the query grammar is
LL(1): one token of lookahead always decides the production.

Notation is ISO/IEC 14977 EBNF: `=` defines, `,` concatenates, `|` separates alternatives,
`[ ]` is optional, `{ }` is zero or more repetitions, `( )` groups, `-` excludes, `;` ends a rule,
terminals are quoted, `(* *)` is a comment.

### 1.2 Lexical grammar

```
letter    = "A" | "B" | ... | "Z" | "a" | "b" | ... | "z" ;
digit     = "0" | "1" | ... | "9" ;
IDENT     = ( letter | "_" ) , { letter | digit | "_" } ;
NUMBER    = [ "-" ] , digit , { digit } , [ "." , digit , { digit } ] ;
STRING    = "'" , { character - ( "'" | newline ) | "''" } , "'" ;   (* '' is one quote, no newlines *)
SYMBOL    = "(" | ")" | "[" | "]" | "{" | "}" | "," | "." | "="
          | "!=" | "<" | "<=" | ">" | ">=" ;
comment   = "//" , { character - newline } ;                       (* to the end of the line *)
```

Whitespace (space, tab, carriage return, newline) and comments separate tokens and are otherwise
ignored. Three lexical decisions are worth stating because the test cases depend on them:

- Maximal munch. On `<`, `>` or `!` the scanner looks at the next character before deciding, so
  `>=` is one token and `>` `-30` is two (`-` followed by a digit starts a NUMBER, it is never an
  operator). A lone `!` is a lexical error.
- There is no unary minus in the language. `Age > - 30` is a lexical error at the `-`.
- Every token carries its line and column (1-based). Errors report the position of the token, and an
  unterminated string reports the position of its opening quote.

### 1.3 Query grammar

```
Query    = Expr ;                                              (* then end of input *)
Expr     = Inter , { ( "union" | "minus" ) , Inter } ;
Inter    = Product , { "intersect" , Product } ;
Product  = Primary , { ( "times" | "join" , "[" , Cond , "]" ) , Primary } ;
Primary  = "select" , "[" , Cond , "]" , "(" , Expr , ")"
         | "project" , "[" , AttrList , "]" , "(" , Expr , ")"
         | "rename" , "[" , IDENT , "]" , "(" , Expr , ")"
         | "(" , Expr , ")"
         | IDENT ;
AttrList = Attr , { "," , Attr } ;                             (* at least one attribute *)
Cond     = AndC , { "or" , AndC } ;
AndC     = NotC , { "and" , NotC } ;
NotC     = "not" , NotC | Cmp ;
Cmp      = "(" , Cond , ")" | Operand , CmpOp , Operand ;
CmpOp    = "=" | "!=" | "<" | "<=" | ">" | ">=" ;
Operand  = NUMBER | STRING | Attr ;
Attr     = IDENT , [ "." , IDENT ] ;                           (* Name or Relation.Name *)
```

### 1.4 Relation definition grammar

Definition files are line oriented, exactly as the assignment describes them: a header line, one
tuple per line, a closing brace on its own line. Blank lines and lines starting with `//` are ignored.

```
Defs     = { Def } ;
Def      = Header , newline , { Tuple , newline } , "}" , newline ;
Header   = IDENT , "(" , IDENT , { "," , IDENT } , ")" , "=" , "{" ;
Tuple    = Value , { "," , Value } ;                           (* exactly as many values as attributes *)
Value    = STRING | BARE ;
BARE     = { character - ( "," | " " | tab | "(" | ")" | "'" | newline ) }- ;   (* one or more characters *)
```

A BARE value is a number if the whole value matches NUMBER, otherwise it is a string. So `32` is a
number, `'32'` is a string, `D-1` and `2024-01-01` are strings, and anything containing a comma, a
space, a parenthesis or a quote must be quoted, which is the rule in section 4.1 of the assignment.
Two more cases follow from the line rules rather than from the assignment: a bare value may not start
with `//` (the line would be a comment) and may not be exactly `}` (the line would close the relation);
quote them. A relation is a set: duplicate tuples collapse to one when the file is loaded.

### 1.5 Keywords (test case 8)

The tokenizer has no keyword table. It produces IDENT for every word, and the parser treats a word
as a keyword only in the position where the grammar expects that keyword:

| word | is a keyword only... | grammar rule |
|---|---|---|
| `select` `project` `rename` | at the start of a Primary | Primary |
| `union` `minus` | after a complete Inter | Expr |
| `intersect` | after a complete Product | Inter |
| `times` `join` | after a complete Primary | Product |
| `or` | after a complete AndC | Cond |
| `and` | after a complete NotC | AndC |
| `not` | at the start of a condition term | NotC |

Everywhere else a word is an identifier. `select[union=3](R)` therefore parses with an attribute
called `union`, because Operand never expects a keyword. Two consequences follow from the table and
are deliberate: a relation named `select`, `project` or `rename` cannot be queried (the word is taken
as an operator), and an attribute named `not` cannot start a comparison (`select[not=3](R)` is a
syntax error, `select[3=not](R)` is fine). Keywords are lowercase and identifiers are case sensitive.

## 2 Precedence and associativity

Every operator, from tightest to loosest. Precedence lives in the grammar: each level is its own
nonterminal and only calls the next tighter one, so a looser operator can never appear inside a
tighter one without parentheses. Associativity also lives in the grammar. The rule that defines each binary
level is left recursive, which is what makes its operators left associative: the only way to derive
`a op b op c` is to derive `a op b` first, as the left operand.

```
Expr    = Expr , ( "union" | "minus" ) , Inter | Inter ;
Inter   = Inter , "intersect" , Product | Product ;
Product = Product , "times" , Primary | Product , "join" , "[" , Cond , "]" , Primary | Primary ;
Cond    = Cond , "or" , AndC | AndC ;
AndC    = AndC , "and" , NotC | NotC ;
```

Section 1.3 writes the same five rules in the repetition form `Tighter , { op , Tighter }`, the
standard rewrite that removes the left recursion (section 4) and generates exactly the same strings.
The parser's loop builds the node for each repetition with the accumulated left operand, so
`a op b op c` is always `(a op b) op c`, the tree the left recursive rule derives.

| level | operators | associativity | enforced by |
|---|---|---|---|
| 1 (tightest) | comparison `=` `!=` `<` `<=` `>` `>=` | none (a comparison has exactly two operands) | Cmp |
| 2 | `not` | right (prefix, `not not c` is `not (not c)`) | NotC |
| 3 | `and` | left | AndC |
| 4 | `or` | left | Cond |
| 5 | `( Expr )`, `select[...]`, `project[...]`, `rename[...]`, relation name | none (each is self delimiting) | Primary |
| 6 | `times`, `join[...]` | left | Product |
| 7 | `intersect` | left | Inter |
| 8 (loosest) | `union`, `minus` | left | Expr |

Levels 1 to 4 are the condition sublanguage inside `[...]`; levels 5 to 8 are the relation
expressions. The relational levels are the same as the RelaX calculator's precedence table
(projection and selection, then cross product and joins, then intersection, then union and
difference, all left associative), so queries can be checked against it. Consequences:

- `A union B minus C` is `(A union B) minus C` (test case 10).
- `A minus B minus C` is `(A minus B) minus C` (test case 11).
- `A union B intersect C` is `A union (B intersect C)`.
- `A intersect B times C` is `A intersect (B times C)`.
- `A join[c] B times C` is `(A join[c] B) times C`.
- `not a=1 and b=2 or c=3` is `((not a=1) and b=2) or c=3`; `a=1 and b=2 or c=3` is `(a=1 and b=2) or c=3` (test case 13).

The trees the program prints for cases 10 and 11:

```
$ ./ra --tree "A union B minus C"        $ ./ra --tree "A minus B minus C"
Minus                                    Minus
├── Union                                ├── Minus
│   ├── Relation(A)                      │   ├── Relation(A)
│   └── Relation(B)                      │   └── Relation(B)
└── Relation(C)                          └── Relation(C)
```

Data instance where the other grouping of case 11 gives a different answer, using the relations in
`tests/data.txt` (`A = {1, 2, 3}`, `B = {2, 3, 4}`, `C = {3}`):

| query | grouping | result |
|---|---|---|
| `A minus B minus C` | `(A minus B) minus C` = `{1} minus {3}` | `{1}` |
| `A minus (B minus C)` | `A minus {2, 4}` | `{1, 3}` |

Both are tests (`x14`, `x15` in `tests/test.cpp`).

## 3 Ambiguity demonstration

The naive grammar from the assignment:

```
Expr ::= Expr "union" Expr
       | Expr "minus" Expr
       | "(" Expr ")"
       | IDENT
```

It is ambiguous: the input `A union B minus C` has two different parse trees. Drawn in the same style
as `ra --tree`:

```
tree 1: minus at the root                tree 2: union at the root
Minus                                    Union
├── Union                                ├── Relation(A)
│   ├── Relation(A)                      └── Minus
│   └── Relation(B)                          ├── Relation(B)
└── Relation(C)                              └── Relation(C)
meaning (A union B) minus C              meaning A union (B minus C)
```

Tree 1 uses the productions `Expr → Expr minus Expr` at the root and `Expr → Expr union Expr` on the
left child. Tree 2 uses `Expr → Expr union Expr` at the root and `Expr → Expr minus Expr` on the right
child. Both derive the same token string, which is the definition of ambiguity.

A concrete instance where the two trees give different results, again `A = {1, 2, 3}`,
`B = {2, 3, 4}`, `C = {3}`:

| tree | evaluation | result |
|---|---|---|
| 1, `(A union B) minus C` | `{1, 2, 3, 4} minus {3}` | `{1, 2, 4}` |
| 2, `A union (B minus C)` | `{1, 2, 3} union {2, 4}` | `{1, 2, 3, 4}` |

The element 3 is removed by tree 1 and kept by tree 2. The program reproduces both
(`tests/test.cpp` cases `x12` and `x13`, the second written with explicit parentheses).

The stratified grammar that removes the ambiguity:

```
Expr    = Expr , ( "union" | "minus" ) , Primary | Primary ;
Primary = "(" , Expr , ")" | IDENT ;
```

The fix is to stop an operator from sitting between two `Expr`. Its right operand is now a `Primary`,
so a `union` or `minus` can only appear on the right inside parentheses. The only remaining way to
derive a chain is to grow it on the left. `A union B minus C` therefore has exactly one parse tree:
`Expr → Expr minus Primary(C)`, whose left `Expr` is `Expr → Expr union Primary(B)`, whose left `Expr`
is `Primary(A)`. That is tree 1, `(A union B) minus C`. Tree 2 would need `B minus C` as the right
operand of `union`, and `Primary` cannot derive it.

This rule is left recursive, which a recursive descent parser cannot use directly (section 4). The
parser uses the equivalent repetition form, which generates the same strings:

```
Expr    = Primary , { ( "union" | "minus" ) , Primary } ;
```

Its loop builds each new node with the tree so far as the left operand, which is the same tree 1. The
full grammar in sections 1.3 and 2 has the same shape, with `Inter` in place of `Primary`.

## 4 Parsing strategy

The parser is a hand-written recursive descent parser: one C++ method per nonterminal
(`query`, `expr`, `inter`, `product`, `primary` with its `unary` helper, `attrList`, `attr`, `cond`,
`andc`, `notc`, `cmp`, `operand` in `src/parser.hpp`), each consuming tokens from a vector produced by the hand-written
scanner in `src/lexer.hpp`. It is predictive with one token of lookahead (LL(1)): every method looks
at the current token, decides which alternative to take, and never backtracks. That is possible
because the grammar was stratified first (section 2) and because every alternative of every rule
starts with a distinct token (`select`, `project`, `rename`, `(`, or an identifier in `Primary`;
`(`, or an operand in `Cmp`; `not`, or anything else in `NotC`).

The reason for the strategy is that the grammar is small, the tree is exactly the call tree, and
error messages fall out naturally: whichever method fails to find the token it expects reports
`expected X but found Y` with the position of the offending token (cases 16 and 17).

Left recursion is the thing a recursive descent parser cannot survive. The naive rule
`Expr ::= Expr "union" Expr` would make `expr()` call `expr()` as its very first action without
consuming a token, which recurses until the stack overflows. The five left recursive rules in
section 2 would do exactly that, so section 1.3 replaces each of them by the `first , { op , next }`
shape: `Expr` calls `Inter` first, `Inter` calls
`Product`, `Product` calls `Primary`, and only `Primary` recurses into `Expr`, after consuming a
`(` or a keyword. The same shape appears in `Cond`, `AndC` and `Product`. The loop body in each
method (`n = Op(n, next())`) is what folds the repetition to the left, so removing the left
recursion did not change the associativity that the naive grammar's left tree had.

The parse tree is built bottom up as the methods return and is printed without executing anything
(`ra --tree`). Evaluation walks the same tree bottom up in `src/engine.hpp`.

## 5 Sources, and where AI assistance was wrong

Read for this document and the parser:

- Robert Nystrom, *Crafting Interpreters*, chapter 4 "Scanning" (character by character scanning,
  the `match` lookahead for two-character operators, unterminated strings) and chapter 6 "Parsing
  Expressions" (stratifying a grammar into precedence levels, why left recursion breaks recursive
  descent, the `{ op , next }` rewrite). https://craftinginterpreters.com/scanning.html and
  https://craftinginterpreters.com/parsing-expressions.html
- Wikipedia, "Extended Backus–Naur form" (the ISO 14977 notation used above),
  "Recursive descent parser" (LL(k), predictive parsers, left recursion),
  "Maximal munch" (longest match, and the `x=y/*z` and `>>` counterexamples),
  "Operator-precedence parser" (stratified grammars versus precedence climbing).
- Aho, Lam, Sethi, Ullman, *Compilers: Principles, Techniques, and Tools*, sections 2.2 to 2.4
  (context-free grammars, parse trees, ambiguity, associativity and precedence, recursive descent)
  and 4.4 (top-down parsing, eliminating left recursion, LL(1)).
- RelaX relational algebra calculator, help page, operator precedence table
  (https://relax.mad.uom.gr/help.htm), and the calculator itself for checking self joins and set
  operations by hand.

Where AI assistance was wrong during this project (each was found by a failing test or by reading
the output, details and dates in `DESIGN_LOG.md`):

1. The first engine it wrote bound condition attributes with a free function called `bind`. Because
   one argument was a `std::vector`, argument-dependent lookup silently selected `std::bind` from
   `<functional>` instead, the returned bind object was discarded, and every attribute resolved to
   column 0. Joins returned the full cross product and `select[zz=3](R)` returned rows instead of a
   name error. The test suite caught it (27 failures); an instrumented build showed the function was
   never entered. Renamed to `bindCond`.
2. Its first deduplication compacted rows in place with `rows[n++] = std::move(rows[i])`, which is a
   self move assignment when no duplicate has been seen yet. libc++ frees the vector's own buffer and
   the program crashed with a segmentation fault on the first query that executed. Rewritten to build
   a new vector.
3. Its first tuple scanner had `i++` in the `for` header and also advanced `i` past the comma inside
   the loop, so the first character of every value after the first was skipped and `32` loaded as
   `2`. Case 14 failed with a confusing `expected a value` syntax error deep inside `tests/data.txt`.
4. Its first draft of the data file rule allowed bare strings only in identifier shape. That
   contradicts section 4.1 of the assignment, which says a string needs quotes only if it contains a
   comma, a space, a parenthesis or a quote, so `D-1` or `2024-01-01` would have been rejected. Found
   by checking the draft against the spec sentence by sentence; fixed with the BARE rule in 1.4.
5. It proposed implementing `intersect` and `minus` with `std::binary_search`, relying on an
   invariant that every relation is kept sorted. The invariant is true today but silent, and the
   assignment says nested loops are expected; a plain nested loop with our own tuple equality is
   what the code uses.
6. It cited a RelaX help URL (`dbis-uibk.github.io/relax/help`) that does not exist. The precedence
   table is at `relax.mad.uom.gr/help.htm`.
7. Its parser built each binary node with a braced initializer list, `{n, inter()}`, which copies the
   whole left subtree at every step of the left fold, so a chain of 30000 `union`s took 43 seconds
   to parse. Found by a reviewer timing long chains; the nodes are now moved into their parent.
8. Its recursive descent had no depth limit, so 5000 nested parentheses overflowed the stack and the
   process died with a segmentation fault and no message, which section 6.3 forbids. A depth counter
   at the four places where a construct can contain itself (parentheses in expressions and in
   conditions, the three unary operators, `not`) now turns that into `syntax error: nesting too deep`,
   and a second check refuses any tree deeper than 500 levels, because a chain of 25000 `and`s built a
   tree that overflowed the evaluator's stack the same way.
