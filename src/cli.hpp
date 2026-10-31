// what the command line tools share: options, reading files, and rows and schemas as JSON
#pragma once
#include "catalog.hpp"
#include "json.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>

// a command, then --name value options and bare --flags
struct Args {
    std::string command;
    std::map<std::string, std::string> options;
    std::set<std::string> flags;

    bool has(const std::string& flag) const { return flags.count(flag) > 0; }
    const std::string& need(const std::string& name) const {
        auto it = options.find(name);
        if (it == options.end()) throw Error{"error: " + command + " needs --" + name};
        return it->second;
    }
    std::string get(const std::string& name, const std::string& fallback) const { auto it = options.find(name); return it == options.end() ? fallback : it->second; }
    int number(const std::string& name, int fallback) const {
        auto it = options.find(name);
        if (it == options.end()) return fallback;
        const std::string& s = it->second;
        bool digits = !s.empty() && s.size() < 10 && std::all_of(s.begin(), s.end(), dig);
        if (!digits) throw Error{"error: --" + name + " must be a non-negative integer, not '" + s + "'"};
        return std::stoi(s);
    }
    // a misspelt option would otherwise be ignored without a word
    void allow(const std::set<std::string>& names) const {
        for (const auto& [name, value] : options) if (!names.count(name)) throw Error{"error: " + command + " does not take --" + name};
        for (const std::string& flag : flags) if (!names.count(flag)) throw Error{"error: " + command + " does not take --" + flag};
    }
};
inline Args parseArgs(const std::vector<std::string>& args, const std::set<std::string>& flagNames) {
    Args a;
    if (!args.empty()) a.command = args[0];
    for (size_t i = 1; i < args.size(); i++) {
        const std::string& s = args[i];
        if (s.compare(0, 2, "--") != 0) throw Error{"error: unexpected argument '" + s + "'"};
        std::string name = s.substr(2);
        if (flagNames.count(name)) a.flags.insert(name);
        else if (i + 1 < args.size()) a.options[name] = args[++i];
        else throw Error{"error: --" + name + " needs a value"};
    }
    return a;
}
inline std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f || std::filesystem::is_directory(path)) throw Error{"io error: cannot open " + path};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
inline Policy policyNamed(const std::string& name) {
    if (name != "lru" && name != "clock") throw Error{"error: --policy must be lru or clock, not '" + name + "'"};
    return name == "lru" ? Lru : Clock;
}

inline Json valueJson(const Value& v) { return v.isNum ? jsonNumber(v.text) : jsonString(v.text); }
inline Json rowJson(const Row& row) { Json j = jsonArray(); for (const Value& v : row) j.push(valueJson(v)); return j; }
inline Json schemaJson(const std::vector<Col>& cols, bool qualify) {
    Json j = jsonArray();
    for (const Col& c : cols) {
        Json col = jsonArray();
        col.push(jsonString(qualify ? qualified(c) : c.name)).push(jsonString(c.type == StrT ? "VARCHAR" : c.type == RealT ? "REAL" : "INT"));
        if (c.type == StrT) col.push(jsonNumber(c.length));
        j.push(col);
    }
    return j;
}
inline Json statsJson(const PoolStats& s) {
    return jsonObject().add("hits", jsonNumber((long long)s.hits)).add("misses", jsonNumber((long long)s.misses)).add("evictions", jsonNumber((long long)s.evictions))
                       .add("reads", jsonNumber((long long)s.reads)).add("writes", jsonNumber((long long)s.writes));
}
inline std::string statsText(const PoolStats& s) {
    return "hits=" + std::to_string(s.hits) + " misses=" + std::to_string(s.misses) + " evictions=" + std::to_string(s.evictions) +
           " reads=" + std::to_string(s.reads) + " writes=" + std::to_string(s.writes);
}
// errors are one line on standard output and exit code 1, as in Bonus Project 1
inline int report(const Error& e, std::ostream& out) {
    out << e.msg;
    if (e.line) out << " at line " << e.line << " col " << e.col;
    out << '\n';
    return 1;
}
