// the parse tree written as the logical plan JSON the modules exchange, and read back
#pragma once
#include "json.hpp"
#include "parser.hpp"

inline Json treeToJson(const Node& n) {
    static const char* const OP[] = {"scan", "union", "intersect", "except", "cross", "distinct", "select", "project", "rename", "join"};
    Json j = jsonObject();
    j.add("op", jsonString(OP[n.kind]));
    switch (n.kind) {
        case Relation: j.add("table", jsonString(n.val.text)); break;
        case Distinct: j.add("child", treeToJson(n.kids[0])); break;
        case Select: j.add("condition", jsonString(condText(n.kids[0]))).add("child", treeToJson(n.kids[1])); break;
        case Project: {
            Json columns = jsonArray();
            for (const Node& a : n.kids[0].kids) columns.push(jsonString(a.val.text));
            j.add("columns", columns).add("child", treeToJson(n.kids[1]));
            break;
        }
        case Rename: j.add("to", jsonString(n.kids[0].val.text)).add("child", treeToJson(n.kids[1])); break;
        case Join: j.add("condition", jsonString(condText(n.kids[0]))).add("left", treeToJson(n.kids[1])).add("right", treeToJson(n.kids[2])); break;
        default: j.add("left", treeToJson(n.kids[0])).add("right", treeToJson(n.kids[1])); break;
    }
    return j;
}

inline const Json& member(const Json& j, const std::string& key) {
    const Json* v = j.find(key);
    if (!v) throw Error{"plan error: missing field '" + key + "'", j.line, j.col};
    return *v;
}
inline Node jsonToTree(const Json& j) {
    const std::string& op = member(j, "op").text;
    Token t{Word, op, j.line, j.col};
    auto word = [&](const std::string& text) { return Token{Word, text, j.line, j.col}; };
    if (op == "scan") return mk(Relation, word(member(j, "table").text));
    if (op == "distinct") return mk(Distinct, t, jsonToTree(member(j, "child")));
    if (op == "select") return mk(Select, t, parseCondition(member(j, "condition").text), jsonToTree(member(j, "child")));
    if (op == "project") {
        Node list = mk(List, t);
        for (const Json& c : member(j, "columns").items) list.kids.push_back(mk(Attr, word(c.text)));
        return mk(Project, t, std::move(list), jsonToTree(member(j, "child")));
    }
    if (op == "rename") return mk(Rename, t, mk(Relation, word(member(j, "to").text)), jsonToTree(member(j, "child")));
    if (op == "join") return mk(Join, t, parseCondition(member(j, "condition").text), jsonToTree(member(j, "left")), jsonToTree(member(j, "right")));
    Kind k = op == "cross" ? Times : op == "union" ? Union : op == "intersect" ? Intersect : op == "except" ? Minus : List;
    if (k == List) throw Error{"plan error: unknown operator '" + op + "'", j.line, j.col};
    return mk(k, word(k == Times ? "times" : k == Minus ? "minus" : op), jsonToTree(member(j, "left")), jsonToTree(member(j, "right")));
}
