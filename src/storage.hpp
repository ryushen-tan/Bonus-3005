// the storage command line tool, Module 1
#pragma once
#include "cli.hpp"
#include "datafile.hpp"
#include <iomanip>

inline std::string pad(const std::string& s, size_t width) { return s.size() >= width ? s : std::string(width - s.size(), ' ') + s; }
inline std::string pad(long long n, size_t width) { return pad(std::to_string(n), width); }
inline std::string rowText(const Row& row) { std::string s; for (size_t i = 0; i < row.size(); i++) s += (i ? ", " : "") + show(row[i]); return s; }

// slot by slot with byte offsets, for a data page, or entry by entry for a directory page
inline void layout(Database& db, Table& t, uint32_t pageNo, std::ostream& out) {
    uint32_t total = db.disk.numPages(t.file);
    if (pageNo >= total) throw Error{"error: " + t.fileName + " has " + std::to_string(total) + " pages, there is no page " + std::to_string(pageNo)};
    bool directory = false;
    for (uint32_t dirNo = 0; dirNo != NO_PAGE && !directory;) {
        const uint8_t* dir = db.pool.fetch({t.file, dirNo});
        uint32_t next = nextDirPage(dir);
        db.pool.unpin({t.file, dirNo}, false);
        directory = dirNo == pageNo;
        dirNo = next;
    }
    const uint8_t* page = db.pool.fetch({t.file, pageNo});
    out << t.fileName << " page " << pageNo << ", " << (directory ? "directory" : "data") << " page\n";
    out << "offset  size  field\n";
    if (directory) {
        uint32_t next = nextDirPage(page);
        out << pad(0, 6) << pad(4, 6) << "  page_id        " << pageIdOf(page) << '\n';
        out << pad(4, 6) << pad(4, 6) << "  next_dir_page  " << (next == NO_PAGE ? "none (0xFFFFFFFF)" : std::to_string(next)) << '\n';
        out << pad(8, 6) << pad(2, 6) << "  entry_count    " << entryCount(page) << '\n';
        out << "entry  at offset  data_page  free_bytes\n";
        for (int i = 0; i < entryCount(page); i++)
            out << pad(i, 5) << pad(DIR_HEADER + DIR_ENTRY * i, 11) << pad(entryPage(page, i), 11) << pad(entryFree(page, i), 12) << '\n';
    } else {
        out << pad(0, 6) << pad(4, 6) << "  page_id     " << pageIdOf(page) << '\n';
        out << pad(4, 6) << pad(2, 6) << "  slot_count  " << slotCount(page) << '\n';
        out << pad(6, 6) << pad(2, 6) << "  free_end    " << freeEnd(page) << '\n';
        out << "free space " << freeSpace(page) << " bytes, from offset " << DATA_HEADER + SLOT_SIZE * slotCount(page) << " to " << freeEnd(page) << '\n';
        out << "slot  at offset  record offset  length  record\n";
        for (int i = 0; i < slotCount(page); i++) {
            out << pad(i, 4) << pad(DATA_HEADER + SLOT_SIZE * i, 11) << pad(slotOffset(page, i), 15) << pad(slotLength(page, i), 8) << "  ";
            if (live(page, i)) out << '(' << rowText(decode(t.cols, recordAt(page, i))) << ")\n";
            else out << "dead\n";
        }
    }
    db.pool.unpin({t.file, pageNo}, false);
}

inline std::vector<uint32_t> readTrace(const std::string& text) {
    std::vector<uint32_t> ids;
    std::istringstream in(text);
    std::string line;
    for (int lineNo = 1; std::getline(in, line); lineNo++) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (line.size() > 9 || !std::all_of(line.begin(), line.end(), dig)) throw Error{"error: trace line " + std::to_string(lineNo) + ": '" + line + "' is not a page id"};
        ids.push_back(uint32_t(std::stoul(line)));
    }
    return ids;
}
// fetches each page and unpins it clean straight away, on a scratch file large enough for every id
inline PoolStats runTrace(const std::string& dir, const std::vector<uint32_t>& ids, int frames, Policy policy) {
    std::filesystem::create_directories(dir);
    DiskManager disk;
    uint32_t file = disk.createTemp(dir);
    uint32_t largest = 0;
    for (uint32_t id : ids) largest = std::max(largest, id);
    disk.truncate(file, ids.empty() ? 0 : largest + 1);
    BufferPool pool(disk, frames, policy);
    for (uint32_t id : ids) { pool.fetch({file, id}); pool.unpin({file, id}, false); }
    PoolStats s = pool.stats();
    disk.remove(file);
    return s;
}

inline int storageTool(const std::vector<std::string>& args, std::ostream& out) {
    const char* usage =
        "usage: storage create-table --db DIR --name R --schema \"a:INT,b:VARCHAR(20)\"\n"
        "       storage load   --db DIR --table R --file r.csv [--frames B]\n"
        "       storage scan   --db DIR --table R [--frames B] [--limit N] [--policy lru|clock] [--json]\n"
        "       storage layout --db DIR --table R --page P\n"
        "       storage trace  --db DIR --file trace.txt [--frames B] [--policy lru|clock] [--json]\n";
    try {
        Args a = parseArgs(args, {"json"});
        if (a.command == "create-table") {
            a.allow({"db", "name", "schema"});
            Database db(a.need("db"), 8);
            Table& t = db.createTable(a.need("name"), parseSchema(a.need("schema")));
            out << "created table " << t.name << " (";
            for (size_t i = 0; i < t.cols.size(); i++) out << (i ? ", " : "") << t.cols[i].name << ' ' << typeName(t.cols[i]);
            out << ") in " << db.dir << '/' << t.fileName << '\n';
        }
        else if (a.command == "load") {
            a.allow({"db", "table", "file", "frames"});
            Database db(a.need("db"), a.number("frames", 64));
            Table& t = db.table(a.need("table"));
            long rows = loadCsv(db, t, readFile(a.need("file")));
            out << "loaded " << rows << " rows into " << t.name << ", now " << t.liveTuples << " tuples on " << t.dataPages << " data pages\n";
        }
        else if (a.command == "scan") {
            a.allow({"db", "table", "frames", "limit", "policy", "json"});
            int limit = a.options.count("limit") ? a.number("limit", 0) : -1;
            Database db(a.need("db"), a.number("frames", 64), policyNamed(a.get("policy", "lru")));
            Table& t = db.table(a.need("table"));
            std::vector<Row> rows;
            {
                HeapScan scan(db.pool, t.file);
                Rid rid;
                std::string_view record;
                while ((limit < 0 || long(rows.size()) < limit) && scan.next(rid, record)) rows.push_back(decode(t.cols, record));
            }
            if (a.has("json")) {
                Json rowsJson = jsonArray();
                for (const Row& r : rows) rowsJson.push(rowJson(r));
                out << dumpJson(jsonObject().add("table", jsonString(t.name)).add("schema", schemaJson(t.cols, false)).add("rows", rowsJson).add("stats", statsJson(db.pool.stats()))) << '\n';
            } else {
                for (size_t i = 0; i < t.cols.size(); i++) out << (i ? ", " : "") << t.cols[i].name;
                out << '\n';
                for (const Row& r : rows) out << rowText(r) << '\n';
                out << rows.size() << " rows, " << statsText(db.pool.stats()) << '\n';
            }
        }
        else if (a.command == "layout") {
            a.allow({"db", "table", "page"});
            a.need("page");
            Database db(a.need("db"), 8);
            layout(db, db.table(a.need("table")), uint32_t(a.number("page", 0)), out);
        }
        else if (a.command == "trace") {
            a.allow({"db", "file", "frames", "policy", "json"});
            std::string policy = a.get("policy", "lru");
            int frames = a.number("frames", 16);
            if (frames < 1) throw Error{"error: --frames must be at least 1"};
            PoolStats s = runTrace(a.need("db"), readTrace(readFile(a.need("file"))), frames, policyNamed(policy));
            if (a.has("json"))
                out << dumpJson(jsonObject().add("policy", jsonString(policy)).add("frames", jsonNumber(frames)).add("hits", jsonNumber((long long)s.hits)).add("misses", jsonNumber((long long)s.misses))
                                    .add("evictions", jsonNumber((long long)s.evictions)).add("reads", jsonNumber((long long)s.reads)).add("writes", jsonNumber((long long)s.writes))) << '\n';
            else {
                double total = double(s.hits + s.misses);
                out << "policy=" << policy << " frames=" << frames << ' ' << statsText(s) << " hit_rate=" << std::fixed << std::setprecision(4) << (total ? s.hits / total : 0.0) << '\n';
            }
        }
        else { out << usage; return 2; }
        return 0;
    } catch (const Error& e) {
        return report(e, out);
    }
}
