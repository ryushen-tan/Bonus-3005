// the 25 required cases in order, then extras
#include "../src/run.hpp"
#include <iostream>
#include <sstream>

struct Case { const char* name; std::vector<std::string> args; std::string want; int code = 0; long long cmp = -1; };

const std::string F = "tests/data.txt";
const std::string T1 = "Select(cond=Eq(Attr(x1), Num(3)))\n└── Relation(R)\n";
const std::string RH = "x1, Age, Name, a, b, c, A, B\n", R1 = "1, 32, Bob, 1, 2, 4, 5, 5\n", R2 = "2, 28, 'Bob)', 1, 3, 2, 6, 7\n",
                  R3 = "3, 30, 'a,b', 2, 2, 3, 8, 8\n", R4 = "4, -30, 'O''Brien', 0, 0, 0, 1, 2\n";
const std::string JH = "R.x1, R.Age, R.Name, R.a, R.b, R.c, R.A, R.B, S.b, S.c\n";

const Case CASES[] = {
    {"01 no whitespace", {"--tree", "select[x1=3](R)"}, T1},
    {"02 whitespace gives the same tree", {"--tree", "select[ x1 = 3 ](R)"}, T1},
    {"03 >= is one token", {"--tokens", "select[Age>=30](R)"},
     "word select (1:1)\nsymbol [ (1:7)\nword Age (1:8)\nsymbol >= (1:11)\nnumber 30 (1:13)\nsymbol ] (1:15)\nsymbol ( (1:16)\nword R (1:17)\nsymbol ) (1:18)\nend end of input (1:19)\n"},
    {"04 > then -30", {"--tokens", "select[Age>-30](R)"},
     "word select (1:1)\nsymbol [ (1:7)\nword Age (1:8)\nsymbol > (1:11)\nnumber -30 (1:12)\nsymbol ] (1:15)\nsymbol ( (1:16)\nword R (1:17)\nsymbol ) (1:18)\nend end of input (1:19)\n"},
    {"05 parenthesis inside string", {"--tree", "select[Name='Bob)'](R)"}, "Select(cond=Eq(Attr(Name), Str('Bob)')))\n└── Relation(R)\n"},
    {"06 comma inside string", {"--tree", "select[Name='a,b'](R)"}, "Select(cond=Eq(Attr(Name), Str('a,b')))\n└── Relation(R)\n"},
    {"07 doubled quote", {"--tree", "select[Name='O''Brien'](R)"}, "Select(cond=Eq(Attr(Name), Str('O''Brien')))\n└── Relation(R)\n"},
    {"08 keyword as attribute", {"--tree", "select[union=3](R)"}, "Select(cond=Eq(Attr(union), Num(3)))\n└── Relation(R)\n"},
    {"09 unterminated string", {"--tree", "select[Name='Bob](R)"}, "lexical error: unterminated string at line 1 col 13\n", 1},
    {"10 union minus groups left", {"--tree", "A union B minus C"}, "Minus\n├── Union\n│   ├── Relation(A)\n│   └── Relation(B)\n└── Relation(C)\n"},
    {"11 minus minus groups left", {"--tree", "A minus B minus C"}, "Minus\n├── Minus\n│   ├── Relation(A)\n│   └── Relation(B)\n└── Relation(C)\n"},
    {"12 not before and before or", {"--tree", "select[not (a=1 and b=2) or c>3](R)"},
     "Select(cond=Or(Not(And(Eq(Attr(a), Num(1)), Eq(Attr(b), Num(2)))), Gt(Attr(c), Num(3))))\n└── Relation(R)\n"},
    {"13 and before or", {"--tree", "select[a=1 and b=2 or c=3](R)"},
     "Select(cond=Or(And(Eq(Attr(a), Num(1)), Eq(Attr(b), Num(2))), Eq(Attr(c), Num(3))))\n└── Relation(R)\n"},
    {"14 three levels", {F, "project[Name](select[Age>30](select[DID='D1'](Employees)))"}, "Name\nJohn\n", 0, 5},
    {"15 explicit parentheses", {"--tree", "(A union B) minus (C intersect D)"},
     "Minus\n├── Union\n│   ├── Relation(A)\n│   └── Relation(B)\n└── Intersect\n    ├── Relation(C)\n    └── Relation(D)\n"},
    {"16 missing parenthesis", {"--tree", "select[Age>30](R"}, "syntax error: expected ')' but found end of input at line 1 col 17\n", 1},
    {"17 empty attribute list", {"--tree", "project[](R)"}, "syntax error: expected attribute name but found ']' at line 1 col 9\n", 1},
    {"18 attribute against attribute", {F, "select[A=B](R)"}, RH + R1 + R3, 0, 4},
    {"19 qualified join", {F, "Emp join[Emp.DID=Dept.DID] Dept"},
     "Emp.EID, Emp.Name, Emp.MgrID, Emp.DID, Dept.DID, Dept.DName\nE1, John, E3, D1, D1, Sales\nE2, Alice, E1, D2, D2, Eng\nE3, Bob, E0, D1, D1, Sales\n", 0, 9},
    {"20 self join", {F, "rename[E2](Emp) join[Emp.MgrID=E2.EID] Emp"},
     "E2.EID, E2.Name, E2.MgrID, E2.DID, Emp.EID, Emp.Name, Emp.MgrID, Emp.DID\nE1, John, E3, D1, E2, Alice, E1, D2\nE3, Bob, E0, D1, E1, John, E3, D1\n", 0, 9},
    {"21 incompatible union", {F, "R union S"}, "schema error: union inputs are not union compatible: different number of attributes at line 1 col 3\n", 1},
    {"22 number against string", {F, "select[Age>'30'](R)"}, "type error: cannot compare number Age with string '30' at line 1 col 11\n", 1},
    {"23 project removes duplicates", {F, "project[DID](Employees)"}, "DID\nD1\nD2\n"},
    {"24 duplicate projection", {F, "project[Name, Name](R)"}, "schema error: duplicate attribute R.Name in project at line 1 col 1\n", 1},
    {"25 empty result", {F, "select[Age>100](Employees)"}, "EID, Name, Age, DID\n", 0, 3},

    // the tokenizer cases executed
    {"x01 case 1 executed", {F, "select[x1=3](R)"}, RH + R3, 0, 4},
    {"x02 case 3 executed", {F, "select[Age>=30](R)"}, RH + R1 + R3, 0, 4},
    {"x03 case 4 executed", {F, "select[Age>-30](R)"}, RH + R1 + R2 + R3, 0, 4},
    {"x04 case 5 executed", {F, "select[Name='Bob)'](R)"}, RH + R2},
    {"x05 case 6 executed", {F, "select[Name='a,b'](R)"}, RH + R3},
    {"x06 case 7 executed and requoted", {F, "select[Name='O''Brien'](R)"}, RH + R4},
    {"x07 decimal literal", {F, "select[Age>29.5](R)"}, RH + R1 + R3, 0, 4},
    {"x08 minus needs a digit", {"--tree", "select[Age > - 30](R)"}, "lexical error: unexpected character '-' at line 1 col 14\n", 1},
    {"x09 lone bang", {"--tree", "select[Age!30](R)"}, "lexical error: unexpected character '!' at line 1 col 11\n", 1},
    {"x10 bare word is an attribute", {F, "select[Name=Bob](R)"}, "name error: unknown attribute Bob at line 1 col 13\n", 1},
    {"x11 quoted keyword is not a keyword", {"--tree", "select[a=1 'or' b=2](R)"}, "syntax error: expected ']' but found string 'or' at line 1 col 12\n", 1},

    // grammar cases executed
    {"x12 case 10 executed", {F, "A union B minus C"}, "x\n1\n2\n4\n"},
    {"x13 case 10 other grouping differs", {F, "A union (B minus C)"}, "x\n1\n2\n3\n4\n"},
    {"x14 case 11 executed", {F, "A minus B minus C"}, "x\n1\n"},
    {"x15 case 11 other grouping differs", {F, "A minus (B minus C)"}, "x\n1\n3\n"},
    {"x16 case 12 executed", {F, "select[not (a=1 and b=2) or c>3](R)"}, RH + R1 + R2 + R3 + R4, 0, 4},
    {"x17 case 12 other grouping differs", {F, "select[not ((a=1 and b=2) or c>3)](R)"}, RH + R2 + R3 + R4, 0, 4},
    {"x18 case 13 executed", {F, "select[a=1 and b=2 or c=3](R)"}, RH + R1 + R3, 0, 4},
    {"x19 case 13 other grouping differs", {F, "select[a=1 and (b=2 or c=3)](R)"}, RH + R1, 0, 4},
    {"x20 case 15 executed", {F, "(A union B) minus (C intersect D)"}, "x\n1\n2\n4\n"},
    {"x21 intersect binds tighter than union", {"--tree", "A union B intersect C"}, "Union\n├── Relation(A)\n└── Intersect\n    ├── Relation(B)\n    └── Relation(C)\n"},
    {"x22 times binds tighter than intersect", {"--tree", "A intersect B times C"}, "Intersect\n├── Relation(A)\n└── Times\n    ├── Relation(B)\n    └── Relation(C)\n"},
    {"x23 rename tree", {"--tree", "rename[X](project[a, R.b](R))"}, "Rename(name=X)\n└── Project(attrs=[a, R.b])\n    └── Relation(R)\n"},
    {"x24 missing bracket", {"--tree", "select[x1=3(R)"}, "syntax error: expected ']' but found '(' at line 1 col 12\n", 1},
    {"x25 missing operand", {"--tree", "A union"}, "syntax error: expected relation name but found end of input at line 1 col 8\n", 1},
    {"x26 trailing garbage", {"--tree", "A union B C"}, "syntax error: unexpected 'C' at line 1 col 11\n", 1},
    {"x27 missing comparison", {"--tree", "select[a](R)"}, "syntax error: expected comparison operator but found ']' at line 1 col 9\n", 1},
    {"x28 empty query", {"--tree", ""}, "syntax error: expected relation name but found end of input at line 1 col 1\n", 1},

    // operators and schemas
    {"x29 duplicates collapse on load", {F, "D"}, "x\n3\n4\n"},
    {"x30 intersect", {F, "A intersect B"}, "x\n2\n3\n"},
    {"x31 minus to empty", {F, "A minus A"}, "x\n"},
    {"x32 union ignores qualifiers", {F, "rename[X](A) union A"}, "x\n1\n2\n3\n"},
    {"x33 type incompatible union", {F, "A union T"}, "schema error: union inputs are not union compatible: attribute x has different types at line 1 col 3\n", 1},
    {"x34 name incompatible union", {F, "S union Dept"}, "schema error: union inputs are not union compatible: attribute 1 is b on the left and DID on the right at line 1 col 3\n", 1},
    {"x35 join in miniature", {F, "R join[R.b=S.b] S"}, JH + "1, 32, Bob, 1, 2, 4, 5, 5, 2, 10\n2, 28, 'Bob)', 1, 3, 2, 6, 7, 3, 20\n3, 30, 'a,b', 2, 2, 3, 8, 8, 2, 10\n", 0, 8},
    {"x36 join is times then select", {F, "select[R.b=S.b](R times S)"}, JH + "1, 32, Bob, 1, 2, 4, 5, 5, 2, 10\n2, 28, 'Bob)', 1, 3, 2, 6, 7, 3, 20\n3, 30, 'a,b', 2, 2, 3, 8, 8, 2, 10\n", 0, 8},
    {"x37 counter sums over the tree", {F, "select[S.c>10](R join[R.b=S.b] S)"}, JH + "2, 28, 'Bob)', 1, 3, 2, 6, 7, 3, 20\n", 0, 11},
    {"x38 empty join", {F, "Emp join[Emp.DID=Dept.DName] Dept"}, "Emp.EID, Emp.Name, Emp.MgrID, Emp.DID, Dept.DID, Dept.DName\n", 0, 9},
    {"x39 qualified project after join", {F, "project[Dept.DName](Emp join[Emp.DID=Dept.DID] Dept)"}, "DName\nEng\nSales\n", 0, 9},
    {"x40 same name different relations", {F, "project[Emp.DID, Dept.DID](Emp join[Emp.DID=Dept.DID] Dept)"}, "Emp.DID, Dept.DID\nD1, D1\nD2, D2\n", 0, 9},
    {"x41 projection keeps listed order", {F, "project[Name, Age](Employees)"}, "Name, Age\nAlice, 28\nBob, 29\nJohn, 32\n"},
    {"x42 qualified name on a base relation", {F, "select[Employees.Age>30](Employees)"}, "EID, Name, Age, DID\nE1, John, 32, D1\n", 0, 3},
    {"x43 wrong qualifier", {F, "select[Foo.Age>30](Employees)"}, "name error: unknown attribute Foo.Age at line 1 col 8\n", 1},
    {"x44 unknown relation", {F, "select[a=1](Nope)"}, "name error: unknown relation Nope at line 1 col 13\n", 1},
    {"x45 unknown attribute", {F, "select[zz=3](R)"}, "name error: unknown attribute zz at line 1 col 8\n", 1},
    {"x46 ambiguous attribute", {F, "select[c>0](R join[R.b=S.b] S)"}, "name error: ambiguous attribute c at line 1 col 8\n", 1},
    {"x47 times collides without rename", {F, "Emp times Emp"}, "schema error: duplicate attribute Emp.EID in times at line 1 col 5\n", 1},
    {"x48 rename fixes the collision", {F, "project[M.Name](rename[M](Emp) times Emp)"}, "Name\nAlice\nBob\nJohn\n"},
    {"x49 rename can collide", {F, "rename[X](Emp join[Emp.DID=Dept.DID] Dept)"}, "schema error: duplicate attribute X.DID in rename at line 1 col 1\n", 1},
    {"x50 attribute against attribute of another type", {F, "select[Name=Age](R)"}, "type error: cannot compare string Name with number Age at line 1 col 12\n", 1},
    {"x51 tree and result together", {"--tree", F, "C"}, "Relation(C)\nx\n3\n"},

    // data files
    {"x52 bare values", {"tests/bare.txt", "V"}, "id, when, tag\nD-1, 2024-01-01, '30'\nx, 1E5, ''\n"},
    {"x53 wrong arity", {"tests/bad_arity.txt", "R"}, "syntax error: R has 2 attributes but this tuple has 1 at line 3 col 1\n", 1},
    {"x54 mixed column types", {"tests/bad_type.txt", "R"}, "type error: attribute a of R mixes numbers and strings at line 3 col 1\n", 1},
    {"x55 bare value with a space", {"tests/bad_bare.txt", "R"}, "lexical error: bare value must be quoted at line 2 col 3\n", 1},
    {"x56 missing data file", {"nope.txt", "A"}, "io error: cannot open nope.txt\n", 1},
    {"x57 usage", {}, "usage: ra [--tree] [--tokens] [--stats] [datafile] \"query\"\n", 2},
    {"x58 case 8 executed", {F, "select[union=3](K)"}, "union, x\n3, a\n", 0, 2},
    {"x59 comment and newline inside a query", {"--tree", "A // the rest of this line is ignored\n union B"}, "Union\n├── Relation(A)\n└── Relation(B)\n"},
    {"x60 500 nested parentheses parse", {"--tree", std::string(500, '(') + "R" + std::string(500, ')')}, "Relation(R)\n"},
    {"x61 501 nested parentheses are an error not a crash", {"--tree", std::string(501, '(') + "R" + std::string(501, ')')}, "syntax error: nesting too deep at line 1 col 501\n", 1},
    {"x62 number out of range", {"--tree", "select[a=" + std::string(400, '9') + "](R)"}, "lexical error: number out of range at line 1 col 10\n", 1},
    {"x63 odd strings are quoted on output", {"tests/odd.txt", "O"}, "s\n''\n' '\n'//c'\n'1'\n'a b'\n'}'\n"},
    {"x64 directory as data file", {"src", "A"}, "io error: cannot open src\n", 1},
    {"x65 missing closing brace", {"tests/bad_brace.txt", "R"}, "syntax error: missing '}' for R at line 3 col 1\n", 1},
    {"x66 left operand is checked first", {F, "Nope1 union Nope2"}, "name error: unknown relation Nope1 at line 1 col 1\n", 1},
    {"x67 chain of 499 operators parses", {"A" + [] { std::string s; for (int i = 0; i < 499; i++) s += " union A"; return s; }()}, ""},
    {"x68 chain of 500 operators is too deep", {"A" + [] { std::string s; for (int i = 0; i < 500; i++) s += " union A"; return s; }()}, "syntax error: nesting too deep at line 1 col 3995\n", 1},
    {"x69 long condition chain is too deep not a crash", {F, "select[" + [] { std::string s; for (int i = 0; i < 600; i++) s += "a=1 and "; return s + "a=1](R)"; }()}, "syntax error: nesting too deep at line 1 col 3996\n", 1},
    {"x70 unknown flag", {"--help"}, "usage: ra [--tree] [--tokens] [--stats] [datafile] \"query\"\n", 2},
    {"x71 text after the closing brace", {"tests/bad_brace2.txt", "R"}, "syntax error: unexpected text after '}' at line 3 col 2\n", 1},
};

int main() {
    int total = std::size(CASES), fails = 0;
    for (const Case& c : CASES) {
        std::ostringstream o;
        Engine e;
        int rc = run(c.args, o, e);
        if (rc == c.code && o.str() == c.want && (c.cmp < 0 || (long long)e.comparisons == c.cmp)) continue;
        fails++;
        std::cout << "FAIL " << c.name << " (exit " << rc << ", comparisons " << e.comparisons << ")\n--- want ---\n" << c.want << "--- got ---\n" << o.str();
    }
    std::cout << total - fails << "/" << total << " passed\n";
    return fails != 0;
}
