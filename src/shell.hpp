// the integrated system: SQL in, rows out, through all four modules
#pragma once
#include "opt.hpp"
#include <unistd.h>

// statements end at a semicolon that is not inside a string or a comment; the positions of those semicolons
inline std::vector<size_t> statementEnds(const std::string& text) {
    std::vector<size_t> out;
    bool quote = false;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (quote) { if (c == '\'') quote = false; continue; }
        if (c == '\'') quote = true;
        else if (text.compare(i, 2, "--") == 0) i = std::min(text.find('\n', i), text.size());
        else if (c == ';') out.push_back(i);
    }
    return out;
}
inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
// the first position from i on that is not blank, in a comment, or the semicolon that ends the statement
inline size_t skipBlank(const std::string& s, size_t i) {
    while (i < s.size()) {
        if (s.compare(i, 2, "--") == 0) i = std::min(s.find('\n', i), s.size());
        else if (std::string(" \t\r\n;").find(s[i]) != std::string::npos) i++;
        else break;
    }
    return i;
}
// the words of a shell command: a quoted string kept whole, = a word of its own, and comments left out
inline std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = skipBlank(s, 0); i < s.size(); i = skipBlank(s, i)) {
        size_t j = i + 1;
        if (s[i] == '\'') { j = s.find('\'', i + 1); j = j == std::string::npos ? s.size() : j + 1; }
        else if (s[i] != '=') while (j < s.size() && skipBlank(s, j) == j && s[j] != '=') j++;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

struct Shell {
    std::string dir;
    int frames;
    std::unique_ptr<Database> db;
    std::ostream& out;
    // where in the whole input the next statement starts, so that errors give positions in the input
    int line = 1, column = 1;

    Shell(std::string directory, int f, std::ostream& o) : dir(std::move(directory)), frames(f), db(std::make_unique<Database>(dir, f)), out(o) {}

    LogicalNode plan(const Query& q) {
        SqlCompiler compiler = compilerFor(*db);
        Optimizer o{*db, frames, {}, {}};
        return o.optimize(compiler.query(q).node);
    }
    static std::string cell(const Value& v) { return v.text; }
    void printRows(const Result& r) {
        std::vector<size_t> width;
        for (const Col& c : r.schema) width.push_back(c.name.size());
        for (const Row& row : r.rows) for (size_t i = 0; i < row.size(); i++) width[i] = std::max(width[i], cell(row[i]).size());
        for (size_t i = 0; i < r.schema.size(); i++) out << (i ? " | " : " ") << r.schema[i].name << std::string(width[i] - r.schema[i].name.size(), ' ');
        out << '\n';
        for (size_t i = 0; i < r.schema.size(); i++) out << (i ? "-+-" : "-") << std::string(width[i], '-');
        out << "-\n";
        for (const Row& row : r.rows) {
            for (size_t i = 0; i < row.size(); i++) {
                std::string text = cell(row[i]);
                std::string gap(width[i] - text.size(), ' ');
                out << (i ? " | " : " ") << (row[i].isNum ? gap + text : text + gap);
            }
            out << '\n';
        }
    }
    void select(const Query& q, bool analyze) {
        LogicalNode p = plan(q);
        if (!analyze) {
            std::unique_ptr<Operator> root = buildPlan(*db, physicalJson(p));
            Result r = execute(*db, *root);
            printRows(r);
            Counters t = totals(*root);
            char time[32];
            std::snprintf(time, sizeof time, "%.3f", r.seconds * 1000);
            out << "(" << r.rows.size() << (r.rows.size() == 1 ? " row" : " rows") << ")  page I/O " << t.io.reads + t.io.writes << " (" << t.io.reads << " reads, " << t.io.writes
                << " writes)  time " << time << " ms  rows estimated " << (long long)p.rows << " actual " << r.rows.size() << '\n';
            return;
        }
        std::unique_ptr<Operator> root = buildPlan(*db, physicalJson(p));
        Result r = execute(*db, *root);
        std::vector<std::pair<std::string, const LogicalNode*>> lines;
        explainLines(p, 0, lines);
        std::vector<const Operator*> ops;
        std::function<void(const Operator&)> walk = [&](const Operator& o) { ops.push_back(&o); for (const auto& c : o.children) walk(*c); };
        walk(*root);
        size_t width = 0;
        for (const auto& l : lines) width = std::max(width, l.first.size());
        for (size_t i = 0; i < lines.size(); i++) {
            const LogicalNode& n = *lines[i].second;
            const Operator& o = *ops[i];
            double est = n.rows, actual = double(o.counters.tuplesOut);
            double q = std::max(est, actual) / std::max(1.0, std::min(est, actual));
            char qerror[32];
            std::snprintf(qerror, sizeof qerror, "%.2f", actual == 0 && est <= 1 ? 1.0 : q);
            out << lines[i].first << std::string(width + 2 - lines[i].first.size(), ' ') << "rows est=" << (long long)est << " actual=" << o.counters.tuplesOut << " q=" << qerror
                << "  cost est=" << std::llround(n.cost) << " actual=" << o.counters.io.reads + o.counters.io.writes << '\n';
        }
        Counters t = totals(*root);
        char time[32];
        std::snprintf(time, sizeof time, "%.3f", r.seconds * 1000);
        out << "total: rows " << r.rows.size() << ", page I/O estimated " << std::llround(p.total) << " actual " << t.io.reads + t.io.writes << " (" << t.io.reads << " reads, " << t.io.writes
            << " writes), time " << time << " ms, peak frames " << r.peakFrames << " of " << frames << '\n';
    }
    void command(const std::string& text) {
        std::vector<std::string> w = words(text);
        std::string first = w.empty() ? "" : upper(w[0]);
        if (first == "EXPLAIN") {
            bool analyze = w.size() > 1 && upper(w[1]) == "ANALYZE";
            size_t start = skipBlank(text, 0) + w[0].size();
            if (analyze) start = skipBlank(text, start) + w[1].size();
            // the words before the query are blanked rather than cut off, so the query keeps its positions
            std::string query = text;
            for (size_t i = 0; i < start; i++) if (query[i] != '\n') query[i] = ' ';
            std::vector<Statement> s = parseSql(query);
            if (s.size() != 1 || s[0].kind != Statement::Select) throw Error{"error: EXPLAIN takes one SELECT"};
            if (analyze) select(*s[0].query, true);
            else printExplain(plan(*s[0].query), out);
        }
        else if (first == "ANALYZE") {
            bool withBuckets = w.size() == 3 && w[2].size() < 10 && std::all_of(w[2].begin(), w[2].end(), dig);
            if (w.size() != 2 && !withBuckets) throw Error{"error: write ANALYZE table [buckets], with buckets from 1 to 999999999"};
            Table& t = db->table(w[1]);
            analyzeTable(*db, t, withBuckets ? std::stoi(w[2]) : 20);
            out << "ANALYZE " << t.name << ": " << t.stats.tuples << " tuples, " << t.stats.pages << " pages\n";
        }
        else if (first == "SET") {
            if (w.size() != 4 || upper(w[1]) != "FRAMES" || w[2] != "=" || !std::all_of(w[3].begin(), w[3].end(), dig) || w[3].size() > 6) throw Error{"error: write SET frames = n"};
            int n = std::stoi(w[3]);
            if (n < MIN_PLAN_FRAMES) throw Error{"error: frames must be at least " + std::to_string(MIN_PLAN_FRAMES)};
            // the database is reopened before the open one is let go, so a failure leaves the shell as it was
            db = std::make_unique<Database>(dir, n);
            frames = n;
            out << "SET frames = " << frames << '\n';
        }
        else if (first == "LOAD") {
            if (w.size() != 4 || upper(w[2]) != "FROM") throw Error{"error: write LOAD table FROM 'file'"};
            std::string path = w[3].size() >= 2 && w[3][0] == '\'' ? w[3].substr(1, w[3].size() - 2) : w[3];
            Table& t = db->table(w[1]);
            long rows = loadCsv(*db, t, readFile(path));
            out << "LOAD " << rows << " rows into " << t.name << '\n';
        }
        else for (const Statement& s : parseSql(text)) statement(s);
    }
    void statement(const Statement& s) {
        SqlCompiler compiler = compilerFor(*db);
        if (s.kind == Statement::Select) { select(*s.query, false); return; }
        if (s.kind == Statement::Create) {
            Table& t = db->createTable(s.table, compiler.createTable(s));
            out << "CREATE TABLE " << t.name << '\n';
            return;
        }
        Row row = compiler.insertRow(s);
        Table& t = db->table(s.table);
        HeapFile heap = db->heap(t);
        heap.insert(encode(t.cols, row));
        db->saveCatalog();
        out << "INSERT 1\n";
    }
    // runs one statement, which starts at line and column of the input; an error stops that statement only
    bool runStatement(const std::string& text) {
        try {
            command(text);
            return true;
        } catch (const SqlError& e) {
            out << sqlErrorText({e.category, e.message, line + e.line - 1, e.line == 1 ? column + e.col - 1 : e.col}) << '\n';
        } catch (const Error& e) {
            report(e, out);
        } catch (const std::exception& e) {
            out << "internal error: " << e.what() << '\n';
        }
        return false;
    }
    // runs every statement the text completes, and leaves what follows the last one in rest
    bool run(const std::string& text, std::string& rest) {
        bool ok = true;
        size_t start = 0;
        for (size_t end : statementEnds(text)) {
            ok = runStatement(text.substr(start, end + 1 - start)) && ok;
            for (; start <= end; start++) if (text[start] == '\n') { line++; column = 1; } else column++;
        }
        rest = text.substr(start);
        return ok;
    }
};

inline int shellTool(const std::vector<std::string>& args, std::istream& in, std::ostream& out) {
    try {
        std::vector<std::string> withCommand = {"shell"};
        withCommand.insert(withCommand.end(), args.begin(), args.end());
        Args a = parseArgs(withCommand, {});
        a.allow({"db", "frames", "file"});
        int frames = a.number("frames", 32);
        if (frames < MIN_PLAN_FRAMES) throw Error{"error: --frames must be at least " + std::to_string(MIN_PLAN_FRAMES)};
        Shell shell(a.need("db"), frames, out);
        bool ok = true;
        std::string pending;
        // the text after the last semicolon is a statement too, and is run, or reported, at the end
        if (a.options.count("file")) {
            ok = shell.run(readFile(a.need("file")), pending);
            return shell.runStatement(pending) && ok ? 0 : 1;
        }
        bool interactive = &in == &std::cin && isatty(0);
        std::string line;
        for (;;) {
            if (interactive) out << (trim(pending).empty() ? "db> " : "  -> ") << std::flush;
            if (!std::getline(in, line)) break;
            std::string text = pending + line + "\n";
            ok = shell.run(text, pending) && ok;
        }
        return shell.runStatement(pending) && ok ? 0 : 1;
    } catch (const Error& e) {
        return report(e, out);
    } catch (const std::exception& e) {
        return report(Error{std::string("internal error: ") + e.what()}, out);
    }
}
