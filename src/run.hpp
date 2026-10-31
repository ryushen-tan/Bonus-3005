// command line handling, shared by ra and the tests
#pragma once
#include "datafile.hpp"
#include "treejson.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

inline int run(const std::vector<std::string>& args, std::ostream& out, Engine& e) {
    bool tree = false, tokens = false, json = false, stats = false, badFlag = false;
    std::vector<std::string> pos;
    for (const std::string& a : args)
        if (a == "--tree") tree = true; else if (a == "--tokens") tokens = true; else if (a == "--json") json = true; else if (a == "--stats") stats = true;
        else if (a.compare(0, 2, "--") == 0) badFlag = true; else pos.push_back(a);
    if (badFlag || pos.empty() || pos.size() > 2) { out << "usage: ra [--tree] [--tokens] [--json] [--stats] [datafile] \"query\"\n"; return 2; }
    try {
        std::vector<Token> toks = tokenize(pos.back());
        if (tokens) for (const Token& t : toks) out << TOK[t.kind] << ' ' << t.text << " (" << t.line << ':' << t.col << ")\n";
        Node q = Parser{toks}.query();
        if (tree) printTree(q, out);
        if (json) out << dumpJson(treeToJson(q)) << '\n';
        if (pos.size() == 1) return 0;
        std::ifstream f(pos[0]);
        if (!f || std::filesystem::is_directory(pos[0])) throw Error{"io error: cannot open " + pos[0]};
        std::map<std::string, Rel> db = load(std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()));
        e.source = [&db](const std::string& name) -> const Rel* { auto it = db.find(name); return it == db.end() ? nullptr : &it->second; };
        auto t0 = std::chrono::steady_clock::now();
        Rel r = e.exec(q);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (stats) out << "comparisons=" << e.comparisons << " tuples=" << r.rows.size() << " time=" << secs << '\n';
        else print(r, out);
        return 0;
    } catch (const Error& err) {
        out << err.msg;
        if (err.line) out << " at line " << err.line << " col " << err.col;
        out << '\n';
        return 1;
    } catch (const std::exception& ex) {
        out << "internal error: " << ex.what() << '\n';
        return 1;
    }
}
