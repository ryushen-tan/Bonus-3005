# Relational algebra query processor

COMP 3005 bonus project 1. A hand-written tokenizer, a recursive descent parser, a parse tree
printer, the six relational algebra operators with set semantics, a data generator and a
performance study, in C++17 with no dependencies beyond the standard library.

| file | contents |
|---|---|
| `src/lexer.hpp` | tokens and the character by character scanner |
| `src/parser.hpp` | parse tree nodes, one method per grammar rule, the tree printer |
| `src/engine.hpp` | relations, tuple order and equality, binding, evaluation, the operators, output, loading |
| `src/run.hpp` | the command line, shared by the program and the tests |
| `src/ra.cpp` | `main` |
| `src/gen.cpp` | data generator for the performance study |
| `tests/test.cpp` | the 25 required cases in order, then extras; `tests/*.txt` are their data files |
| `perf/run.sh`, `perf/plot.py` | the experiment driver and the plot |
| `GRAMMAR.md` | grammar, precedence, ambiguity demonstration, parsing strategy, sources |
| `REPORT.md` | the performance study |
| `DESIGN_LOG.md` | dated diary |

## Build and run

Needs `clang++` (or `g++` with `make CXX=g++`) and GNU make.

```
make            builds ./ra and ./gen
make test       builds and runs ./ratest, prints N/N passed
make perf       runs the experiment, writes perf/results.tsv and perf/time.svg, prints the table
```

```
./ra --tree "<query>"                    print the parse tree, nothing is executed
./ra --tokens "<query>"                  print the token stream as  kind text (line:col)
./ra <datafile> "<query>"                load the relations, run the query, print the result
./ra --tree <datafile> "<query>"         both
./ra --stats <datafile> "<query>"        print  comparisons=N tuples=T time=S  instead of the result
./gen <n> <m> <k>                        write R(a, b) with n tuples and S(b, c) with m tuples, each R tuple joining k S tuples
```

```
$ ./ra --tree "project[Name](select[Age>30](Employees))"
Project(attrs=[Name])
└── Select(cond=Gt(Attr(Age), Num(30)))
    └── Relation(Employees)

$ ./ra --tree "rename[E2](Emp) join[Emp.MgrID=E2.EID] select[Name='O''Brien'](Emp)"
Join(cond=Eq(Attr(Emp.MgrID), Attr(E2.EID)))
├── Rename(name=E2)
│   └── Relation(Emp)
└── Select(cond=Eq(Attr(Name), Str('O''Brien')))
    └── Relation(Emp)

$ ./ra tests/data.txt "project[Name](select[Age>30](Employees))"
Name
John
```

Exit code is 0 on success, 1 on any error, 2 on a usage error (including an unknown flag).

## The language

Full grammar, precedence table and the keyword rule are in `GRAMMAR.md`. In short:

```
select[ <condition> ]( <expr> )      project[ <attribute-list> ]( <expr> )      rename[ <name> ]( <expr> )
<expr> union <expr>    <expr> intersect <expr>    <expr> minus <expr>    <expr> times <expr>    <expr> join[ <condition> ] <expr>
```

Conditions combine comparisons (`=` `!=` `<` `<=` `>` `>=`) with `not`, `and`, `or` and
parentheses. An operand is a number, a quoted string, or an attribute, optionally qualified as
`Emp.DID`. A bare word in a condition is always an attribute, so `select[Name=Bob](R)` is a name
error and `select[Name='Bob'](R)` is the comparison with a string.

Precedence, tightest first: `select`/`project`/`rename` and parentheses, then `times` and `join`,
then `intersect`, then `union` and `minus`. Everything is left associative, so
`A union B minus C` is `(A union B) minus C` and `A minus B minus C` is `(A minus B) minus C`.

### Data files

```
// employees and their departments
Employees (EID, Name, Age, DID) = {
  E1, John, 32, D1
  E2, Alice, 28, D2
  E3, Bob, 29, D1
}
```

One header line, one tuple per line with exactly as many values as attributes, and `}` alone on a
line. Blank lines and lines starting with `//` are ignored. A value that contains a comma, a space,
a parenthesis or a quote must be quoted with single quotes, and `''` inside quotes is one quote
(`'O''Brien'`). A bare value is a number if it looks like one (`32`, `-30`, `2.5`), otherwise it is a
string (`E1`, `D-1`, `2024-01-01`); `'32'` is a string. Two more values need quotes because of the
line rules: a value that starts with `//` and the single character `}`. Numbers must fit in a double.
A relation is a set, so duplicate tuples in a file collapse to one.

### Semantics

| operator | output schema and behaviour |
|---|---|
| `select[c](e)` | same schema as `e`; `c` may compare two attributes |
| `project[a, b](e)` | the listed attributes in the listed order, duplicates removed; listing the same attribute twice is a schema error |
| `rename[X](e)` | same attributes, now belonging to relation `X`; a schema error if two attributes would then share a name |
| `e times f` | all attributes of both, each remembered with its relation name; a duplicate qualified name is a schema error |
| `e join[c] f` | `times` followed by `select[c]`, evaluated pair by pair without building the product; the comparison counter is the same for both spellings |
| `union`, `intersect`, `minus` | schema of the left input; the inputs must have the same number of attributes, the same names in the same order (relation qualifiers are ignored) and the same type in each position; the unknown type of an empty relation matches either type and the result column takes the other side's type |

Every attribute is `(relation, name, type)`. A base relation's attributes belong to it; `rename`
changes the relation of all of them; `times` and `join` keep both sides' relation names. In a
condition or a projection list an unqualified name must match exactly one attribute (two matches is
an ambiguity error, none is a name error) and `Rel.name` must match exactly.

Types are inferred from the data: a column is a number column or a string column, or unknown for a
relation with no tuples. A column that mixes both is a type error when the file is loaded. Comparing
a number with a string (`select[Age>'30'](R)`) is a type error, decided before any tuple is
examined. Numbers compare by value (`1` and `1.0` are equal), strings compare bytewise.

### Output

The header lists the attribute names joined by `, `, qualified as `Rel.name` only when the result
mixes attributes of more than one relation. Then one tuple per line, in sorted order, values written
the way the data file would write them (strings are quoted only when they have to be). A result with
no tuples prints only the header. The output is itself a valid tuple body, so it can be pasted back
into a data file.

```
$ ./ra tests/data.txt "Emp join[Emp.DID=Dept.DID] Dept"
Emp.EID, Emp.Name, Emp.MgrID, Emp.DID, Dept.DID, Dept.DName
E1, John, E3, D1, D1, Sales
E2, Alice, E1, D2, D2, Eng
E3, Bob, E0, D1, D1, Sales
```

### Errors

Every error is one line, `<category> error: <message>`, with `at line L col C` when a position is
known, and the program exits with 1. There are no stack traces. The categories:

```
lexical error: unterminated string at line 1 col 13                              select[Name='Bob](R)
lexical error: unexpected character '-' at line 1 col 14                         select[Age > - 30](R)
syntax error: expected ')' but found end of input at line 1 col 17              select[Age>30](R
syntax error: expected attribute name but found ']' at line 1 col 9             project[](R)
syntax error: expected ']' but found string 'or' at line 1 col 12               select[a=1 'or' b=2](R)
name error: unknown attribute zz at line 1 col 8                                select[zz=3](R)
name error: ambiguous attribute c at line 1 col 8                               select[c>0](R join[R.b=S.b] S)
schema error: union inputs are not union compatible: different number of attributes at line 1 col 3     R union S
schema error: duplicate attribute Emp.EID in times at line 1 col 5              Emp times Emp
type error: cannot compare number Age with string '30' at line 1 col 11         select[Age>'30'](R)
io error: cannot open nope.txt
```

Data file problems (wrong number of values on a line, a column mixing numbers and strings, an
unquoted value containing a space, a duplicate relation or attribute, a missing `}`) are reported
the same way with the line number in the file.

## Why the self join needs rename (test case 20)

```
rename[E2](Emp) join[Emp.MgrID=E2.EID] Emp
```

Without `rename` the only way to pair employees with their managers is `Emp times Emp` or
`Emp join[...] Emp`. Both produce a schema whose attributes are `Emp.EID, Emp.Name, Emp.MgrID,
Emp.DID` twice, and the assignment says a collision of qualified names is an error, because a
condition like `Emp.MgrID = Emp.EID` could not say which copy it means (and evaluated within a single
copy, `select[MgrID=EID](Emp)` only finds people who manage themselves). `rename[E2]` gives the second
copy a different relation name, so `E2.EID` and `Emp.EID` are distinct attributes, the product has a
legal schema, and the condition can refer to the manager's id on one side and the employee's manager
field on the other.

## Performance study

`make perf` generates `R(a, b)` and `S(b, c)` at 1000 to 64000 tuples per relation with `./gen`,
runs the join, a selection and a projection at each size with `--stats`, and sweeps the match rate.
The match rate `k` is exact when `k` divides `m`: every run of `k` consecutive S tuples shares one `b`
value and R cycles through those values; `k = 0` gives no matches.
The comparison counter increments once for every pair of tuples the join examines and once for
every tuple a selection examines; it is counted, never estimated. `--stats` reports the counter, the
number of output tuples and the wall time of evaluating the tree (loading, parsing and printing are
excluded). Results and the answers to the questions are in `REPORT.md`.

## Tests

`make test` runs `tests/test.cpp`: a table of `{name, arguments, expected output, exit code,
expected comparison count}`. Each entry calls the same `run` function as `main` with an in-memory
output stream and compares the whole output byte for byte, so the suite is deterministic and needs
no shell. Entries `01` to `25` are the assignment's cases in order; `x01` onwards are extras,
including the "other grouping gives a different answer" checks for cases 10 to 13.

## Known limitations

- No nulls, no arithmetic in conditions, no aggregation, no natural join: the assignment's language
  and nothing more.
- A relation named `select`, `project` or `rename` cannot be queried, and an attribute named `not`
  cannot start a comparison (see the keyword rule in `GRAMMAR.md`).
- Numbers are stored as doubles; integers above 2^53 lose precision when compared.
- Everything is in memory; the join is a nested loop with no indexes, which the performance study
  measures on purpose.
- Two limits stop deep input from overflowing the stack. They produce `syntax error: nesting too deep`
  instead of a crash. First, the parser allows at most 500 parentheses, unary operators and `not`s open
  at once, so 500 nested parentheses parse and 501 do not. Second, a parse tree may be at most 500 nodes
  deep: a chain of 499 `union`s parses and 500 do not, and each unary operator or `not` adds a level too.
- Columns in error positions count bytes, so a multibyte character counts as more than one column.
