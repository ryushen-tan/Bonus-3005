// the catalog, stored in catalog.db as a heap file of typed records, and the database it describes
#pragma once
#include "heap.hpp"
#include <cctype>
#include <deque>
#include <filesystem>
#include <sstream>

struct Bucket { Value lo, hi; long long count, ndv; };
// width is the column's average encoded size in bytes
struct ColumnStats { long long ndv = 0; Value min, max; std::vector<Bucket> histogram; double width = 0; };
struct TableStats { bool analyzed = false; long long tuples = 0, pages = 0; double width = 0; std::vector<ColumnStats> columns; };
struct Table {
    std::string name, fileName;
    std::vector<Col> cols;
    uint32_t dataPages = 0, liveTuples = 0;
    TableStats stats;
    uint32_t file = 0;
};


// one schema per kind of catalog record; the first field of every record is its kind
enum RecordKind { TableRecord = 1, ColumnRecord, TableStatsRecord, ColumnStatsRecord, BucketRecord };
inline std::vector<Col> recordSchema(int kind) {
    auto i = [](const char* n) { return Col{"", n, IntT}; };
    auto s = [](const char* n) { return Col{"", n, StrT, 255}; };
    switch (kind) {
        case TableRecord: return {i("kind"), s("table"), s("file"), i("data_pages"), i("live_tuples")};
        case ColumnRecord: return {i("kind"), s("table"), i("position"), s("name"), i("type"), i("length")};
        case TableStatsRecord: return {i("kind"), s("table"), i("tuples"), i("pages"), s("width")};
        case ColumnStatsRecord: return {i("kind"), s("table"), i("position"), i("ndv"), s("min"), s("max"), s("width")};
        default: return {i("kind"), s("table"), i("position"), i("index"), s("lo"), s("hi"), i("count"), i("ndv")};
    }
}
inline Value textValue(const std::string& s) { return {false, 0, s}; }
inline Value parsedValue(const Col& c, const std::string& text) { return c.type == StrT ? textValue(text) : intValue(std::stoll(text)); }

struct Database {
    std::string dir;
    DiskManager disk;
    BufferPool pool;
    // a deque, so that creating a table never moves the others a caller may be holding
    std::deque<Table> tables;
    uint32_t catalogFile;

    Database(const std::string& directory, int frames, Policy policy = Lru) : dir(directory), pool(disk, frames, policy) {
        std::filesystem::create_directories(dir);
        bool fresh = !std::filesystem::exists(dir + "/catalog.db");
        catalogFile = disk.open(dir + "/catalog.db");
        if (fresh) HeapFile::create(pool, catalogFile);
        else readCatalog();
        pool.clear();
    }
    Database(const Database&) = delete;

    // first fit may put a later record on an earlier page, so the records are read in full and then applied
    // tables first, every column and bucket at the position it records
    void readCatalog() {
        std::vector<Row> records[6];
        {
            HeapScan scan(pool, catalogFile);
            Rid rid;
            std::string_view record;
            while (scan.next(rid, record)) {
                int kind = int32_t(get32((const uint8_t*)record.data()));
                records[kind].push_back(decode(recordSchema(kind), record));
            }
        }
        for (const Row& r : records[TableRecord]) {
            Table t{r[1].text, r[2].text, {}, uint32_t(r[3].num), uint32_t(r[4].num), {}, 0};
            t.file = disk.open(dir + "/" + t.fileName);
            tables.push_back(t);
        }
        for (const Row& r : records[ColumnRecord]) {
            Table& t = table(r[1].text);
            size_t at = size_t(r[2].num);
            if (t.cols.size() <= at) { t.cols.resize(at + 1); t.stats.columns.resize(at + 1); }
            t.cols[at] = {t.name, r[3].text, r[4].num == 0 ? IntT : StrT, int(r[5].num)};
        }
        for (const Row& r : records[TableStatsRecord]) {
            TableStats& s = table(r[1].text).stats;
            s.analyzed = true; s.tuples = (long long)r[2].num; s.pages = (long long)r[3].num; s.width = std::stod(r[4].text);
        }
        for (const Row& r : records[ColumnStatsRecord]) {
            Table& t = table(r[1].text);
            const Col& c = t.cols[size_t(r[2].num)];
            ColumnStats& s = t.stats.columns[size_t(r[2].num)];
            s.ndv = (long long)r[3].num;
            s.width = std::stod(r[6].text);
            if (s.ndv > 0) { s.min = parsedValue(c, r[4].text); s.max = parsedValue(c, r[5].text); }
        }
        for (const Row& r : records[BucketRecord]) {
            Table& t = table(r[1].text);
            const Col& c = t.cols[size_t(r[2].num)];
            std::vector<Bucket>& h = t.stats.columns[size_t(r[2].num)].histogram;
            size_t at = size_t(r[3].num);
            if (h.size() <= at) h.resize(at + 1);
            h[at] = {parsedValue(c, r[4].text), parsedValue(c, r[5].text), (long long)r[6].num, (long long)r[7].num};
        }
    }
    // the whole catalog is small, so a change rewrites catalog.db from the first page
    void saveCatalog() {
        pool.dropFile(catalogFile);
        disk.truncate(catalogFile, 0);
        HeapFile::create(pool, catalogFile);
        uint32_t pages = 1, tuples = 0;
        HeapFile heap{pool, catalogFile, pages, tuples};
        auto put = [&](int kind, Row values) {
            values.insert(values.begin(), intValue(kind));
            heap.insert(encode(recordSchema(kind), values));
        };
        auto num = [](long long n) { return intValue(n); };
        for (const Table& t : tables) {
            put(TableRecord, {textValue(t.name), textValue(t.fileName), num(t.dataPages), num(t.liveTuples)});
            for (size_t i = 0; i < t.cols.size(); i++)
                put(ColumnRecord, {textValue(t.name), num(long(i)), textValue(t.cols[i].name), num(t.cols[i].type == StrT), num(t.cols[i].length)});
            if (!t.stats.analyzed) continue;
            auto exact = [](double d) { char text[32]; std::snprintf(text, sizeof text, "%.17g", d); return textValue(text); };
            put(TableStatsRecord, {textValue(t.name), num(t.stats.tuples), num(t.stats.pages), exact(t.stats.width)});
            for (size_t i = 0; i < t.cols.size(); i++) {
                const ColumnStats& s = t.stats.columns[i];
                put(ColumnStatsRecord, {textValue(t.name), num(long(i)), num(s.ndv), textValue(s.ndv ? s.min.text : ""), textValue(s.ndv ? s.max.text : ""), exact(s.width)});
                for (size_t b = 0; b < s.histogram.size(); b++)
                    put(BucketRecord, {textValue(t.name), num(long(i)), num(long(b)), textValue(s.histogram[b].lo.text), textValue(s.histogram[b].hi.text), num(s.histogram[b].count), num(s.histogram[b].ndv)});
            }
        }
        pool.flushAll();
    }

    Table* find(const std::string& name) {
        for (Table& t : tables) if (t.name == name) return &t;
        return nullptr;
    }
    Table& table(const std::string& name) {
        Table* t = find(name);
        if (!t) throw Error{"error: unknown table " + name};
        return *t;
    }
    Table& createTable(const std::string& name, std::vector<Col> cols) {
        if (find(name)) throw Error{"error: table " + name + " already exists"};
        for (Col& c : cols) c.rel = name;
        checkUnique(cols, 0, 0, "table " + name);
        Table t{name, name + ".db", cols, 0, 0, {}, 0};
        t.stats.columns.resize(cols.size());
        std::filesystem::remove(dir + "/" + t.fileName);
        t.file = disk.open(dir + "/" + t.fileName);
        HeapFile::create(pool, t.file);
        tables.push_back(t);
        saveCatalog();
        return tables.back();
    }
    HeapFile heap(Table& t) { return {pool, t.file, t.dataPages, t.liveTuples}; }
};

// "a:INT,b:VARCHAR(20)", types in any case
inline std::vector<Col> parseSchema(const std::string& text) {
    std::vector<Col> cols;
    size_t i = 0;
    auto fail = [&](const std::string& what) -> Error { return Error{"error: bad schema \"" + text + "\": " + what + " at character " + std::to_string(i + 1)}; };
    for (;;) {
        size_t start = i;
        while (i < text.size() && wordChar(text[i])) i++;
        if (i == start || dig(text[start])) throw fail("expected a column name");
        Col c{"", text.substr(start, i - start), IntT};
        if (i >= text.size() || text[i] != ':') throw fail("expected ':'");
        start = ++i;
        while (i < text.size() && wordChar(text[i])) i++;
        std::string type = text.substr(start, i - start);
        for (char& ch : type) ch = char(std::toupper((unsigned char)ch));
        if (type == "VARCHAR") {
            if (i >= text.size() || text[i] != '(') throw fail("expected '('");
            start = ++i;
            while (i < text.size() && dig(text[i])) i++;
            if (i == start || i >= text.size() || text[i] != ')') throw fail("expected a length and ')'");
            c.type = StrT;
            c.length = std::stoi(text.substr(start, i - start));
            if (c.length < 1 || c.length > 255) throw fail("VARCHAR length must be 1 to 255");
            i++;
        }
        else if (type != "INT") throw fail("expected INT or VARCHAR(n)");
        cols.push_back(c);
        if (i == text.size()) return cols;
        if (text[i] != ',') throw fail("expected ','");
        i++;
    }
}

// a field must fit its column: an INT in 32 bits, a VARCHAR(n) in n bytes
inline Value checkedValue(const Col& c, const std::string& field, const std::string& where) {
    if (c.type == StrT) {
        if (int(field.size()) > c.length) throw Error{"error: " + where + ": '" + field + "' is longer than VARCHAR(" + std::to_string(c.length) + ")"};
        return textValue(field);
    }
    size_t i = field.size() && (field[0] == '-' || field[0] == '+') ? 1 : 0;
    bool digits = i < field.size();
    for (size_t k = i; k < field.size(); k++) digits = digits && dig(field[k]);
    long long n = digits && field.size() <= 11 ? std::stoll(field) : 0;
    if (!digits || field.size() > 11 || n < INT32_MIN || n > INT32_MAX) throw Error{"error: " + where + ": '" + field + "' is not an INT"};
    return intValue(n);
}

// one record per line, comma separated, a field with a comma in double quotes and "" for a quote inside them
inline std::vector<std::string> csvFields(const std::string& line, int lineNo) {
    std::vector<std::string> fields(1);
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == ',') { fields.emplace_back(); continue; }
        if (line[i] != '"' || !fields.back().empty()) { fields.back() += line[i]; continue; }
        for (i++;; i++) {
            if (i >= line.size()) throw Error{"error: line " + std::to_string(lineNo) + ": unterminated quoted field"};
            if (line[i] != '"') fields.back() += line[i];
            else if (i + 1 < line.size() && line[i + 1] == '"') { fields.back() += '"'; i++; }
            else break;
        }
        if (i + 1 < line.size() && line[i + 1] != ',') throw Error{"error: line " + std::to_string(lineNo) + ": text after a quoted field"};
    }
    return fields;
}
// checks every line, and inserts each one when given a heap file
inline long csvPass(const Table& t, const std::string& text, HeapFile* heap) {
    std::istringstream in(text);
    std::string line;
    long count = 0;
    for (int lineNo = 1; std::getline(in, line); lineNo++, count++) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> fields = csvFields(line, lineNo);
        if (fields.size() != t.cols.size())
            throw Error{"error: line " + std::to_string(lineNo) + " has " + std::to_string(fields.size()) + " fields but table " + t.name + " has " + std::to_string(t.cols.size()) + " columns"};
        Row row;
        for (size_t i = 0; i < fields.size(); i++)
            row.push_back(checkedValue(t.cols[i], fields[i], "line " + std::to_string(lineNo) + ", column " + std::to_string(i + 1) + " (" + t.cols[i].name + ")"));
        if (encodedSize(t.cols, row) > size_t(MAX_RECORD))
            throw Error{"error: table " + t.name + ", row " + std::to_string(lineNo) + ": a record of " + std::to_string(encodedSize(t.cols, row)) + " bytes does not fit on a page (at most " + std::to_string(MAX_RECORD) + ")"};
        if (heap) heap->insert(encode(t.cols, row));
    }
    return count;
}
// reads the file once to check every line and again to insert, so a bad file loads nothing
inline long loadCsv(Database& db, Table& t, const std::string& text) {
    csvPass(t, text, nullptr);
    HeapFile heap = db.heap(t);
    long count = csvPass(t, text, &heap);
    db.saveCatalog();
    return count;
}
