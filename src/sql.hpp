// the sql command line tool, Module 3
#pragma once
#include "cli.hpp"
#include "sqlplan.hpp"

// the tables of an existing database, without changing it
inline SqlCompiler compilerFor(Database& db) {
    SqlCompiler c;
    c.tables = [&db](const std::string& name) -> const std::vector<Col>* { Table* t = db.find(name); return t ? &t->cols : nullptr; };
    return c;
}

inline int sqlTool(const std::vector<std::string>& args, std::ostream& out) {
    const char* usage = "usage: sql parse   --query \"SELECT ...\" [--explain]\n"
                        "       sql compile --db DIR --file q.sql [--json]\n"
                        "       sql check   --db DIR --file q.sql\n";
    try {
        Args a = parseArgs(args, {"json", "explain"});
        if (a.command == "parse") {
            a.allow({"query", "explain"});
            for (const Statement& s : parseSql(a.need("query"))) printStatement(s, out);
            return 0;
        }
        if (a.command != "compile" && a.command != "check") { out << usage; return 2; }
        a.allow({"db", "file", "json"});
        std::string text = readFile(a.need("file"));
        std::unique_ptr<Database> db = openDatabase(a.need("db"), 8);
        SqlCompiler compiler = compilerFor(*db);
        std::vector<Json> plans;
        for (const Statement& s : parseSql(text)) plans.push_back(compiler.statementJson(s));
        if (a.command == "check") return 0;
        if (a.has("json")) {
            Json all = jsonArray();
            for (Json& p : plans) all.push(p);
            out << dumpJson(plans.size() == 1 ? plans[0] : all) << '\n';
        } else {
            SqlCompiler again = compilerFor(*db);
            for (const Statement& s : parseSql(text)) {
                if (s.kind == Statement::Select) printLogical(again.query(*s.query).node, out);
                else out << dumpJson(again.statementJson(s), -1) << '\n';
            }
        }
        return 0;
    } catch (const SqlError& e) {
        out << sqlErrorText(e) << '\n';
        return 1;
    } catch (const Error& e) {
        return report(e, out);
    } catch (const std::exception& e) {
        return report(Error{std::string("internal error: ") + e.what()}, out);
    }
}
