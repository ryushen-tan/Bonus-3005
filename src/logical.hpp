// the logical plan: what Module 3 produces and Module 4 consumes, with its JSON form
#pragma once
#include "json.hpp"
#include <array>
#include <ostream>

struct LogicalNode {
    std::string op;
    std::string table, to, condition;
    // project: the column, and its new name or empty
    std::vector<std::pair<std::string, std::string>> columns;
    // sort: the column, and whether it is descending
    std::vector<std::pair<std::string, bool>> sortKeys;
    std::vector<std::string> keys;
    // group_by: function, column or *, output name
    std::vector<std::array<std::string, 3>> aggregates;
    long long n = 0;
    std::vector<LogicalNode> kids;
};

// the name an aggregate's output column gets when the query does not give one: count_star, avg_Emp_salary
inline std::string aggregateName(const std::string& func, const std::string& column) {
    if (column == "*") return func + "_star";
    std::string name = func + "_" + column;
    std::replace(name.begin(), name.end(), '.', '_');
    return name;
}

inline LogicalNode logicalNode(const std::string& op, std::vector<LogicalNode> kids = {}) { LogicalNode n; n.op = op; n.kids = std::move(kids); return n; }

inline Json logicalJson(const LogicalNode& n) {
    Json j = jsonObject();
    j.add("op", jsonString(n.op));
    if (n.op == "scan") j.add("table", jsonString(n.table));
    if (n.op == "rename") j.add("to", jsonString(n.to));
    if (n.op == "select" || n.op == "join") j.add("condition", jsonString(n.condition));
    if (n.op == "project") {
        Json cols = jsonArray();
        for (const auto& [c, alias] : n.columns) cols.push(alias.empty() ? jsonString(c) : jsonArray().push(jsonString(c)).push(jsonString(alias)));
        j.add("columns", cols);
    }
    if (n.op == "sort") {
        Json keys = jsonArray();
        for (const auto& [c, desc] : n.sortKeys) keys.push(jsonArray().push(jsonString(c)).push(jsonString(desc ? "desc" : "asc")));
        j.add("keys", keys);
    }
    if (n.op == "group_by") {
        Json keys = jsonArray(), aggs = jsonArray();
        for (const std::string& k : n.keys) keys.push(jsonString(k));
        for (const auto& a : n.aggregates) aggs.push(jsonArray().push(jsonString(a[0])).push(jsonString(a[1])).push(jsonString(a[2])));
        j.add("keys", keys).add("aggregates", aggs);
    }
    if (n.op == "limit") j.add("n", jsonNumber(n.n));
    if (n.kids.size() == 1) j.add("child", logicalJson(n.kids[0]));
    if (n.kids.size() == 2) j.add("left", logicalJson(n.kids[0])).add("right", logicalJson(n.kids[1]));
    return j;
}

inline std::string logicalLabel(const LogicalNode& n) {
    std::string s = n.op;
    auto list = [](const std::vector<std::string>& xs) { std::string t = "["; for (size_t i = 0; i < xs.size(); i++) t += (i ? ", " : "") + xs[i]; return t + "]"; };
    if (n.op == "scan") s += " " + n.table;
    else if (n.op == "rename") s += " " + n.to;
    else if (n.op == "select" || n.op == "join") s += " " + n.condition;
    else if (n.op == "project") { std::vector<std::string> cols; for (const auto& [c, a] : n.columns) cols.push_back(a.empty() ? c : c + " as " + a); s += " " + list(cols); }
    else if (n.op == "sort") { std::vector<std::string> keys; for (const auto& [c, d] : n.sortKeys) keys.push_back(c + (d ? " desc" : " asc")); s += " " + list(keys); }
    else if (n.op == "group_by") { std::vector<std::string> aggs; for (const auto& a : n.aggregates) aggs.push_back(a[0] + "(" + a[1] + ") as " + a[2]); s += " keys " + list(n.keys) + " aggregates " + list(aggs); }
    else if (n.op == "limit") s += " " + std::to_string(n.n);
    return s;
}
inline void printLogical(const LogicalNode& n, std::ostream& out, int depth = 0) {
    out << std::string(size_t(2 * depth), ' ') << logicalLabel(n) << '\n';
    for (const LogicalNode& k : n.kids) printLogical(k, out, depth + 1);
}
