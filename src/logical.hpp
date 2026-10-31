// the logical plan: what Module 3 produces and Module 4 consumes, with its JSON form
#pragma once
#include "json.hpp"
#include <array>
#include <map>
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
    // filled in by the optimizer: the physical algorithm or mode, and the estimates for explain
    std::string algorithm;
    double rows = 0, pages = 0, cost = 0, total = 0;
};

// the name an aggregate's output column gets when the query does not give one: count_star, avg_Emp_salary
inline std::string aggregateName(const std::string& func, const std::string& column) {
    if (column == "*") return func + "_star";
    std::string name = func + "_" + column;
    std::replace(name.begin(), name.end(), '.', '_');
    return name;
}

inline LogicalNode logicalNode(const std::string& op, std::vector<LogicalNode> kids = {}) { LogicalNode n; n.op = op; n.kids = std::move(kids); return n; }

// a node's operator and its own fields, without its inputs
inline Json logicalFields(const LogicalNode& n) {
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
    return j;
}
inline Json logicalJson(const LogicalNode& n) {
    Json j = logicalFields(n);
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

// ---- reading a logical plan back, with every problem of its shape named by node ----

const std::map<std::string, std::vector<std::string>> LOGICAL_FIELDS = {
    {"scan", {"table"}}, {"select", {"condition", "child"}}, {"project", {"columns", "child"}}, {"rename", {"to", "child"}},
    {"join", {"condition", "left", "right"}}, {"cross", {"left", "right"}}, {"group_by", {"keys", "aggregates", "child"}},
    {"distinct", {"child"}}, {"sort", {"keys", "child"}}, {"limit", {"n", "child"}},
    {"union", {"left", "right"}}, {"union_all", {"left", "right"}}, {"intersect", {"left", "right"}}, {"except", {"left", "right"}},
};
[[noreturn]] inline void logicalError(const Json& j, const std::string& path, const std::string& problem) {
    const Json* op = j.kind == Json::Object ? j.find("op") : nullptr;
    std::string node = op && op->kind == Json::String ? "Node '" + op->text + "' (" + path + ")" : "Node at " + path;
    throw Error{node + ": " + problem + "."};
}
inline LogicalNode logicalFromJson(const Json& j, const std::string& path = "root") {
    if (j.kind != Json::Object) logicalError(j, path, "must be an object");
    const Json* opField = j.find("op");
    if (!opField || opField->kind != Json::String) logicalError(j, path, "missing required field 'op' or it is not a string");
    LogicalNode n;
    n.op = opField->text;
    auto rule = LOGICAL_FIELDS.find(n.op);
    if (rule == LOGICAL_FIELDS.end()) logicalError(j, path, "unknown logical operator '" + n.op + "'");
    for (const std::string& field : rule->second) if (!j.find(field)) logicalError(j, path, "missing required field '" + field + "'");
    auto text = [&](const char* field) {
        const Json& v = *j.find(field);
        if (v.kind != Json::String || v.text.empty()) logicalError(j, path, std::string("field '") + field + "' must be a non-empty string");
        return v.text;
    };
    auto strings = [&](const Json& v, const char* field) {
        if (v.kind != Json::Array || !std::all_of(v.items.begin(), v.items.end(), [](const Json& x) { return x.kind == Json::String; }))
            logicalError(j, path, std::string("field '") + field + "' must be an array of strings");
    };
    if (j.find("table")) n.table = text("table");
    if (j.find("to")) n.to = text("to");
    if (j.find("condition")) n.condition = text("condition");
    if (n.op == "project") {
        const Json& cols = *j.find("columns");
        if (cols.kind != Json::Array || cols.items.empty()) logicalError(j, path, "field 'columns' must be a non-empty array");
        for (const Json& c : cols.items) {
            if (c.kind == Json::String) n.columns.emplace_back(c.text, "");
            else if (c.kind == Json::Array && c.items.size() == 2 && c.items[0].kind == Json::String && c.items[1].kind == Json::String) n.columns.emplace_back(c.items[0].text, c.items[1].text);
            else logicalError(j, path, "field 'columns' must hold column names or [column, alias] pairs");
        }
    }
    if (n.op == "sort") {
        const Json& keys = *j.find("keys");
        if (keys.kind != Json::Array || keys.items.empty()) logicalError(j, path, "field 'keys' must be a non-empty array");
        for (const Json& k : keys.items) {
            bool ok = k.kind == Json::Array && k.items.size() == 2 && k.items[0].kind == Json::String && (k.items[1].text == "asc" || k.items[1].text == "desc");
            if (!ok) logicalError(j, path, "field 'keys' must hold [column, \"asc\" or \"desc\"] pairs");
            n.sortKeys.emplace_back(k.items[0].text, k.items[1].text == "desc");
        }
    }
    if (n.op == "group_by") {
        strings(*j.find("keys"), "keys");
        for (const Json& k : j.find("keys")->items) n.keys.push_back(k.text);
        const Json& aggs = *j.find("aggregates");
        if (aggs.kind != Json::Array) logicalError(j, path, "field 'aggregates' must be an array");
        for (const Json& a : aggs.items) {
            strings(a, "aggregates");
            const std::string f = a.items.empty() ? "" : a.items[0].text;
            bool ok = (a.items.size() == 2 || a.items.size() == 3) && (f == "count" || f == "sum" || f == "avg" || f == "min" || f == "max") && (a.items[1].text != "*" || f == "count");
            if (!ok) logicalError(j, path, "field 'aggregates' must hold [function, column] or [function, column, name]");
            n.aggregates.push_back({f, a.items[1].text, a.items.size() == 3 ? a.items[2].text : aggregateName(f, a.items[1].text)});
        }
    }
    if (n.op == "limit") {
        const Json& v = *j.find("n");
        if (v.kind != Json::Number || !std::all_of(v.text.begin(), v.text.end(), dig) || v.text.size() > 18) logicalError(j, path, "field 'n' must be a non-negative integer");
        n.n = std::stoll(v.text);
    }
    for (const char* field : {"child", "left", "right"})
        if (j.find(field)) n.kids.push_back(logicalFromJson(*j.find(field), path + "." + field));
    return n;
}
