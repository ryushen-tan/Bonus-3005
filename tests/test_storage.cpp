// Module 1: the worked examples, the error table and the checklist
#include "../src/storage.hpp"
#include <functional>
#include <sstream>

const std::string DIR = "tests/tmp-storage";

std::string tool(std::vector<std::string> args) { std::ostringstream out; storageTool(args, out); return out.str(); }
std::string errorOf(const std::function<void()>& f) { try { f(); } catch (const Error& e) { return e.msg; } return "no error"; }
void fresh() { std::filesystem::remove_all(DIR); }
std::string ints(std::initializer_list<long long> xs) { std::string s; for (long long x : xs) s += std::to_string(x) + " "; return s; }

struct Test { const char* name; std::function<std::string()> run; };
// each returns what went wrong, or nothing
const Test TESTS[] = {
    {"3.2 page example, every row of the table", [] {
        std::array<uint8_t, PAGE_SIZE> page;
        initDataPage(page.data(), 1);
        std::string got = ints({slotCount(page.data()), freeEnd(page.data()), freeSpace(page.data())});
        for (int length : {13, 20, 13}) {
            insertRecord(page.data(), std::string(size_t(length), 'x'));
            got += "| " + ints({slotCount(page.data()), freeEnd(page.data()), freeSpace(page.data())});
        }
        deleteRecord(page.data(), 1);
        got += "| " + ints({slotCount(page.data()), freeEnd(page.data()), freeSpace(page.data()), slotOffset(page.data(), 1), slotLength(page.data(), 1)});
        std::string want = "0 4096 4088 | 1 4083 4071 | 2 4063 4047 | 3 4050 4030 | 3 4050 4030 0 0 ";
        return got == want ? "" : "got " + got;
    }},
    {"3.2 layout command after the delete", [] {
        fresh();
        {
            Database db(DIR, 8);
            Table& t = db.createTable("R", parseSchema("a:INT,b:VARCHAR(20),c:INT"));
            HeapFile heap = db.heap(t);
            heap.insert(encode(t.cols, {intValue(42), textValue("bob"), intValue(7)}));
            heap.insert(encode(t.cols, {intValue(43), textValue("abcdefghij"), intValue(9)}));
            heap.insert(encode(t.cols, {intValue(44), textValue("amy"), intValue(1)}));
            heap.remove({1, 1});
            db.saveCatalog();
        }
        std::string got = tool({"layout", "--db", DIR, "--table", "R", "--page", "1"});
        std::string want =
            "R.db page 1, data page\noffset  size  field\n     0     4  page_id     1\n     4     2  slot_count  3\n     6     2  free_end    4050\n"
            "free space 4030 bytes, from offset 20 to 4050\nslot  at offset  record offset  length  record\n"
            "   0          8           4083      13  (42, bob, 7)\n   1         12              0       0  dead\n   2         16           4050      13  (44, amy, 1)\n";
        return got == want ? "" : got;
    }},
    {"3.3 capacity of 13 byte records is 240", [] {
        fresh();
        Database db(DIR, 8);
        Table& t = db.createTable("R", parseSchema("a:INT,b:VARCHAR(20),c:INT"));
        HeapFile heap = db.heap(t);
        for (int i = 0; i < 241; i++) heap.insert(encode(t.cols, {intValue(i), textValue("bob"), intValue(7)}));
        uint8_t* page = db.pool.fetch({t.file, 1});
        int onFirst = slotCount(page);
        db.pool.unpin({t.file, 1}, false);
        return onFirst == 240 && t.dataPages == 2 ? "" : "first page holds " + std::to_string(onFirst) + ", data pages " + std::to_string(t.dataPages);
    }},
    {"5.3 buffer example under LRU", [] {
        fresh();
        PoolStats s = runTrace(DIR, {1, 2, 3, 1, 4, 2}, 3, Lru);
        return s.hits == 1 && s.misses == 5 && s.evictions == 2 && s.reads == 5 && s.writes == 0 ? "" : statsText(s);
    }},
    {"5.3 buffer example under CLOCK", [] {
        fresh();
        PoolStats s = runTrace(DIR, {1, 2, 3, 1, 4, 2}, 3, Clock);
        return s.hits == 2 && s.misses == 4 && s.evictions == 1 && s.reads == 4 && s.writes == 0 ? "" : statsText(s);
    }},
    {"5.3 CLOCK pool contents are [4,2,3]", [] {
        DiskManager disk;
        fresh();
        std::filesystem::create_directories(DIR);
        uint32_t f = disk.createTemp(DIR);
        disk.truncate(f, 5);
        BufferPool pool(disk, 3, Clock);
        for (uint32_t id : {1, 2, 3, 1, 4, 2}) { pool.fetch({f, id}); pool.unpin({f, id}, false); }
        std::string got = ints({pool.frames[0].id.page, pool.frames[1].id.page, pool.frames[2].id.page});
        disk.remove(f);
        return got == "4 2 3 " ? "" : got;
    }},
    {"trace --json has the required fields", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        std::ofstream(DIR + "/t.txt") << "1\n2\n3\n1\n4\n2\n";
        std::string got = tool({"trace", "--db", DIR, "--file", DIR + "/t.txt", "--frames", "3", "--policy", "lru", "--json"});
        std::string want = "{\n  \"policy\": \"lru\",\n  \"frames\": 3,\n  \"hits\": 1,\n  \"misses\": 5,\n  \"evictions\": 2,\n  \"reads\": 5,\n  \"writes\": 0\n}\n";
        return got == want ? "" : got;
    }},
    {"evicting a pinned page fails", [] {
        DiskManager disk;
        fresh();
        std::filesystem::create_directories(DIR);
        uint32_t f = disk.createTemp(DIR);
        disk.truncate(f, 3);
        BufferPool pool(disk, 2, Lru);
        pool.fetch({f, 0});
        pool.fetch({f, 1});
        std::string got = errorOf([&] { pool.fetch({f, 2}); });
        disk.remove(f);
        return got == "error: no unpinned frame available, all 2 frames are pinned" ? "" : got;
    }},
    {"evicting a pinned page fails under CLOCK too", [] {
        DiskManager disk;
        fresh();
        std::filesystem::create_directories(DIR);
        uint32_t f = disk.createTemp(DIR);
        disk.truncate(f, 3);
        BufferPool pool(disk, 2, Clock);
        pool.fetch({f, 0});
        pool.fetch({f, 1});
        std::string got = errorOf([&] { pool.fetch({f, 2}); });
        disk.remove(f);
        return got == "error: no unpinned frame available, all 2 frames are pinned" ? "" : got;
    }},
    {"unpinning at pin count 0 is an error", [] {
        DiskManager disk;
        fresh();
        std::filesystem::create_directories(DIR);
        uint32_t f = disk.createTemp(DIR);
        disk.truncate(f, 1);
        BufferPool pool(disk, 2, Lru);
        pool.fetch({f, 0});
        pool.unpin({f, 0}, false);
        std::string got = errorOf([&] { pool.unpin({f, 0}, false); });
        disk.remove(f);
        return got == "error: unpin of page (" + std::to_string(f) + ", 0) whose pin count is already 0" ? "" : got;
    }},
    {"a dirty page is written once when evicted and not again by flush", [] {
        DiskManager disk;
        fresh();
        std::filesystem::create_directories(DIR);
        uint32_t f = disk.createTemp(DIR);
        BufferPool pool(disk, 1, Lru);
        uint32_t a, b;
        pool.newPage(f, a)[100] = 7;
        pool.unpin({f, a}, true);
        pool.newPage(f, b);
        pool.unpin({f, b}, true);
        unsigned long long afterEvict = disk.total.writes;
        uint8_t back = pool.fetch({f, a})[100];
        pool.unpin({f, a}, false);
        pool.flushAll();
        std::string got = ints({(long long)afterEvict, back, (long long)disk.total.writes});
        disk.remove(f);
        return got == "1 7 2 " ? "" : got;
    }},
    {"reading a dead slot names the rid", [] {
        fresh();
        Database db(DIR, 8);
        Table& t = db.createTable("R", parseSchema("a:INT"));
        HeapFile heap = db.heap(t);
        Rid rid = heap.insert(encode(t.cols, {intValue(1)}));
        heap.remove(rid);
        std::string read = errorOf([&] { heap.get(rid); }), again = errorOf([&] { heap.remove(rid); }), past = errorOf([&] { heap.get({1, 5}); });
        bool ok = read == "error: rid (1, 0) is a deleted record" && again == read && past == "error: no record at rid (1, 5), the page has 1 slots" && db.pool.held == 0;
        return ok ? "" : read + " | " + again + " | " + past;
    }},
    {"a record longer than 4088 bytes names the table and row, and nothing is loaded", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        std::string schema, line;
        for (int i = 0; i < 16; i++) { schema += (i ? "," : "") + std::string("c") + std::to_string(i) + ":VARCHAR(255)"; line += (i ? "," : "") + std::string(255, 'x'); }
        std::ofstream(DIR + "/big.csv") << std::string(15, ',') << "\n" << line << "\n";
        tool({"create-table", "--db", DIR, "--name", "Big", "--schema", schema});
        std::string got = tool({"load", "--db", DIR, "--table", "Big", "--file", DIR + "/big.csv"});
        std::string after = tool({"scan", "--db", DIR, "--table", "Big"});
        bool ok = got == "error: table Big, row 2: a record of 4112 bytes does not fit on a page (at most 4088)\n" && after.find("\n0 rows,") != std::string::npos;
        return ok ? "" : got + after;
    }},
    {"bad CSV fields name the line and the column", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT,b:VARCHAR(3)"});
        std::string got;
        for (std::string text : {"1,ab\nx,ab\n", "1,abcd\n", "1\n", "99999999999,a\n", "2147483648,a\n", "-2147483648,\"a,b\"\n"}) {
            std::ofstream(DIR + "/r.csv") << text;
            got += tool({"load", "--db", DIR, "--table", "R", "--file", DIR + "/r.csv"});
        }
        std::string want =
            "error: line 2, column 1 (a): 'x' is not an INT\n"
            "error: line 1, column 2 (b): 'abcd' is longer than VARCHAR(3)\n"
            "error: line 1 has 1 fields but table R has 2 columns\n"
            "error: line 1, column 1 (a): '99999999999' is not an INT\n"
            "error: line 1, column 1 (a): '2147483648' is not an INT\n"
            "loaded 1 rows into R, now 1 tuples on 1 data pages\n";
        return got == want ? "" : got;
    }},
    {"a quoted field keeps its comma and doubled quote", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT,b:VARCHAR(20)"});
        std::ofstream(DIR + "/r.csv") << "1,\"x, \"\"y\"\"\"\r\n2,\n";
        tool({"load", "--db", DIR, "--table", "R", "--file", DIR + "/r.csv"});
        std::string got = tool({"scan", "--db", DIR, "--table", "R", "--json"});
        return got.find("[1, \"x, \\\"y\\\"\"],\n    [2, \"\"]") != std::string::npos ? "" : got;
    }},
    {"a scan of a table twenty times B pins one page at a time", [] {
        fresh();
        Database db(DIR, 8);
        Table& t = db.createTable("R", parseSchema("a:INT,b:VARCHAR(20),c:INT"));
        HeapFile heap = db.heap(t);
        for (int i = 0; i < 240 * 160; i++) heap.insert(encode(t.cols, {intValue(i), textValue("bob"), intValue(7)}));
        db.saveCatalog();
        db.pool.clear();
        HeapScan scan(db.pool, t.file);
        Rid rid;
        std::string_view record;
        long n = 0;
        while (scan.next(rid, record)) n++;
        PoolStats s = db.pool.stats();
        return n == 240 * 160 && t.dataPages == 160 && db.pool.peak == 1 && s.reads == 161 ? "" : ints({n, t.dataPages, db.pool.peak, (long long)s.reads});
    }},
    {"the same command twice gives identical counters", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT,b:VARCHAR(20)"});
        std::ofstream csv(DIR + "/r.csv");
        for (int i = 0; i < 5000; i++) csv << i << ",name" << i % 97 << '\n';
        csv.close();
        tool({"load", "--db", DIR, "--table", "R", "--file", DIR + "/r.csv"});
        std::string first = tool({"scan", "--db", DIR, "--table", "R", "--frames", "4", "--json"}), second = tool({"scan", "--db", DIR, "--table", "R", "--frames", "4", "--json"});
        std::string tail = first.substr(first.find("\"stats\""));
        return first == second ? "" : tail;
    }},
    {"scan --limit stops pulling", [] {
        std::string got = tool({"scan", "--db", DIR, "--table", "R", "--frames", "4", "--limit", "2", "--json"});
        return got.find("\"rows\": [\n    [0, \"name0\"],\n    [1, \"name1\"]\n  ],\n  \"stats\": {\n    \"hits\": 0,\n    \"misses\": 2,") != std::string::npos ? "" : got;
    }},
    {"deleting every record leaves a scan that returns nothing", [] {
        fresh();
        Database db(DIR, 4);
        Table& t = db.createTable("R", parseSchema("a:INT"));
        HeapFile heap = db.heap(t);
        std::vector<Rid> rids;
        for (int i = 0; i < 3000; i++) rids.push_back(heap.insert(encode(t.cols, {intValue(i)})));
        for (Rid r : rids) heap.remove(r);
        HeapScan scan(db.pool, t.file);
        Rid rid;
        std::string_view record;
        bool any = scan.next(rid, record);
        return !any && t.liveTuples == 0 ? "" : "scan returned a record";
    }},
    {"a second directory page after 681 data pages", [] {
        fresh();
        Database db(DIR, 4);
        Table& t = db.createTable("R", parseSchema("a:INT,b:VARCHAR(255)"));
        HeapFile heap = db.heap(t);
        std::string text(255, 'y');
        for (int i = 0; i < 700 * 15; i++) heap.insert(encode(t.cols, {intValue(i), textValue(text)}));
        uint8_t* dir = db.pool.fetch({t.file, 0});
        uint32_t next = nextDirPage(dir);
        int entries = entryCount(dir);
        db.pool.unpin({t.file, 0}, false);
        HeapScan scan(db.pool, t.file);
        Rid rid;
        std::string_view record;
        long n = 0, last = -1;
        bool ordered = true;
        while (scan.next(rid, record)) { long a = decode(t.cols, record)[0].num; ordered = ordered && a == last + 1; last = a; n++; }
        return entries == 681 && next != NO_PAGE && t.dataPages == 700 && n == 700 * 15 && ordered ? "" : ints({entries, next, t.dataPages, n, ordered});
    }},
    {"the catalog survives a restart, statistics included", [] {
        fresh();
        {
            Database db(DIR, 8);
            Table& t = db.createTable("R", parseSchema("a:INT,b:VARCHAR(7)"));
            t.stats.analyzed = true; t.stats.tuples = 3; t.stats.pages = 1; t.stats.width = 10.5;
            t.stats.columns[0] = {3, intValue(1), intValue(9), {{intValue(1), intValue(4), 2, 2}, {intValue(5), intValue(9), 1, 1}}};
            t.stats.columns[1] = {2, textValue("a"), textValue("it's"), {}};
            db.createTable("S", parseSchema("x:INT"));
        }
        Database db(DIR, 8);
        Table& t = db.table("R");
        const ColumnStats& a = t.stats.columns[0];
        bool ok = db.tables.size() == 2 && t.cols.size() == 2 && t.cols[1].type == StrT && t.cols[1].length == 7 && t.stats.width == 10.5 && a.ndv == 3 &&
                  a.histogram.size() == 2 && a.histogram[1].lo.num == 5 && a.histogram[0].count == 2 && t.stats.columns[1].max.text == "it's" && db.table("S").cols[0].name == "x";
        return ok ? "" : "catalog came back different";
    }},
    {"create-table errors", [] {
        fresh();
        std::string got = tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT,a:INT"}) +
                          tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:FLOAT"}) +
                          tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:VARCHAR(300)"}) +
                          tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:int,b:varchar(2)"}) +
                          tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT"}) +
                          tool({"scan", "--db", DIR, "--table", "Nope"}) +
                          tool({"scan", "--db", DIR, "--table", "R", "--frame", "3"});
        std::string want =
            "schema error: duplicate attribute R.a in table R\n"
            "error: bad schema \"a:FLOAT\": expected INT or VARCHAR(n) at character 8\n"
            "error: bad schema \"a:VARCHAR(300)\": VARCHAR length must be 1 to 255 at character 14\n"
            "created table R (a INT, b VARCHAR(2)) in " + DIR + "/R.db\n"
            "error: table R already exists\n"
            "error: unknown table Nope\n"
            "error: scan does not take --frame\n";
        return got == want ? "" : got;
    }},
};

int main() {
    int fails = 0;
    for (const Test& t : TESTS) {
        std::string problem;
        try { problem = t.run(); } catch (const Error& e) { problem = "threw " + e.msg; }
        if (problem.empty()) continue;
        fails++;
        std::cout << "FAIL " << t.name << "\n" << problem << "\n";
    }
    fresh();
    std::cout << std::size(TESTS) - fails << "/" << std::size(TESTS) << " passed\n";
    return fails != 0;
}
