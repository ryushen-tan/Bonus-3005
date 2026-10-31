// the opt command line tool, Module 4
#pragma once
#include "optimizer.hpp"
#include "sql.hpp"

inline Optimizer optimizerFor(Database& db, const Args& a) {
    Optimizer o{db, a.number("frames", 32), {}, {}};
    if (o.frames < 3) throw Error{"error: --frames must be at least 3"};
    for (char rule : a.get("without-rules", "")) {
        if (rule < '1' || rule > '7') throw Error{"error: --without-rules takes rule numbers 1 to 7, like 26"};
        o.disabled.insert(rule - '0');
    }
    return o;
}

inline int optTool(const std::vector<std::string>& args, std::ostream& out) {
    const char* usage = "usage: opt analyze  --db DIR --table R [--buckets 20] [--frames B]\n"
                        "       opt plan     --db DIR --logical q.json [--frames 32] [--json] [--baseline] [--without-rules 26]\n"
                        "       opt explain  --db DIR --logical q.json [--frames 32] [--baseline] [--without-rules 26] [--rewrites]\n"
                        "       opt dp-table --db DIR --logical q.json [--frames 32]\n";
    try {
        Args a = parseArgs(args, {"json", "baseline", "rewrites"});
        if (a.command == "analyze") {
            a.allow({"db", "table", "buckets", "frames"});
            std::unique_ptr<Database> db = openDatabase(a.need("db"), a.number("frames", 64));
            Table& t = db->table(a.need("table"));
            analyzeTable(*db, t, a.number("buckets", 20));
            out << "analyzed " << t.name << ": " << t.stats.tuples << " tuples on " << t.stats.pages << " pages, average width " << t.stats.width << " bytes\n";
            for (size_t i = 0; i < t.cols.size(); i++) {
                const ColumnStats& s = t.stats.columns[i];
                out << "  " << t.cols[i].name << ": ndv=" << s.ndv << " min=" << (s.ndv ? s.min.text : "-") << " max=" << (s.ndv ? s.max.text : "-") << " buckets=" << s.histogram.size() << '\n';
            }
            return 0;
        }
        if (a.command != "plan" && a.command != "explain" && a.command != "dp-table") { out << usage; return 2; }
        a.allow({"db", "logical", "frames", "json", "baseline", "without-rules", "rewrites"});
        std::unique_ptr<Database> db = openDatabase(a.need("db"), 8);
        Optimizer o = optimizerFor(*db, a);
        LogicalNode given = logicalFromJson(parseJson(readFile(a.need("logical"))));
        o.outputCols(given);
        LogicalNode plan = a.has("baseline") ? o.baseline(given) : o.optimize(given);
        if (a.has("baseline")) o.estimate(plan);
        if (a.command == "plan") out << dumpJson(physicalJson(plan)) << '\n';
        else if (a.command == "dp-table") {
            if (o.dpTable.empty()) out << "there is no join in this plan\n";
            for (const std::string& line : o.dpTable) out << line << '\n';
        } else {
            if (a.has("rewrites")) {
                out << "logical plan as given:\n";
                printLogical(given, out, 1);
                out << "after the rewrites and join ordering:\n";
                printLogical(plan, out, 1);
                out << "physical plan:\n";
            }
            printExplain(plan, out);
        }
        return 0;
    } catch (const Error& e) {
        return report(e, out);
    }
}
