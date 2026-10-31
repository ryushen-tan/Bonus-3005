// Module 1: the worked examples, the error table and the checklist
#include "../src/storage.hpp"
#include <functional>
#include <set>
#include <sstream>

const std::string DIR = "tests/tmp-storage";

std::string tool(std::vector<std::string> args) { std::ostringstream out; storageTool(args, out); return out.str(); }
std::string errorOf(const std::function<void()>& f) { try { f(); } catch (const Error& e) { return e.msg; } return "no error"; }
void fresh() { std::filesystem::remove_all(DIR); }
std::string ints(std::initializer_list<long long> xs) { std::string s; for (long long x : xs) s += std::to_string(x) + " "; return s; }
std::string filesIn(const std::string& dir) {
    std::set<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) names.insert(entry.path().filename().string());
    std::string s;
    for (const std::string& name : names) s += name + " ";
    return s;
}

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
    {"unpinning at pin count 0 is an error, under both policies", [] {
        DiskManager disk;
        fresh();
        std::filesystem::create_directories(DIR);
        uint32_t f = disk.createTemp(DIR);
        disk.truncate(f, 1);
        std::string got;
        for (Policy policy : {Lru, Clock}) {
            BufferPool pool(disk, 2, policy);
            pool.fetch({f, 0});
            pool.unpin({f, 0}, false);
            got += errorOf([&] { pool.unpin({f, 0}, false); }) + "\n";
        }
        disk.remove(f);
        std::string once = "error: unpin of page (" + std::to_string(f) + ", 0) whose pin count is already 0\n";
        return got == once + once ? "" : got;
    }},
    {"unpinning a page that is not in the pool is an error, under both policies", [] {
        DiskManager disk;
        fresh();
        std::filesystem::create_directories(DIR);
        uint32_t f = disk.createTemp(DIR);
        disk.truncate(f, 3);
        std::string got;
        for (Policy policy : {Lru, Clock}) {
            BufferPool pool(disk, 1, policy);
            pool.fetch({f, 0});
            pool.unpin({f, 0}, false);
            pool.fetch({f, 1});
            pool.unpin({f, 1}, false);
            got += errorOf([&] { pool.unpin({f, 0}, false); }) + "\n" + errorOf([&] { pool.unpin({f, 2}, false); }) + "\n";
        }
        disk.remove(f);
        std::string page = "(" + std::to_string(f) + ", ";
        std::string once = "error: unpin of page " + page + "0) which is not in the pool\nerror: unpin of page " + page + "2) which is not in the pool\n";
        return got == once + once ? "" : got;
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
    {"a rid on a directory page or past the data pages is an error, and nothing changes", [] {
        fresh();
        Database db(DIR, 8);
        Table& t = db.createTable("R", parseSchema("a:INT,b:VARCHAR(20),c:INT"));
        HeapFile heap = db.heap(t);
        heap.insert(encode(t.cols, {intValue(42), textValue("bob"), intValue(7)}));
        heap.insert(encode(t.cols, {intValue(43), textValue("amy"), intValue(9)}));
        std::string got = errorOf([&] { heap.get({0, 0}); }) + "\n" + errorOf([&] { heap.remove({0, 0}); }) + "\n" + errorOf([&] { heap.get({7, 0}); }) + "\n";
        uint8_t* dir = db.pool.fetch({t.file, 0});
        int entries = entryCount(dir);
        db.pool.unpin({t.file, 0}, false);
        HeapScan scan(db.pool, t.file);
        Rid rid;
        std::string_view record;
        long n = 0;
        while (scan.next(rid, record)) n++;
        std::string want = "error: no record at rid (0, 0), page 0 is not a data page of table R\nerror: no record at rid (0, 0), page 0 is not a data page of table R\n"
                           "error: no record at rid (7, 0), page 7 is not a data page of table R\n";
        return got == want && entries == 1 && n == 2 && t.liveTuples == 2 && db.pool.held == 0 ? "" : got + ints({entries, n, t.liveTuples});
    }},
    {"slots of 2^31 and above are out of range, not negative", [] {
        fresh();
        Database db(DIR, 8);
        Table& t = db.createTable("R", parseSchema("a:INT,b:VARCHAR(20),c:INT"));
        HeapFile heap = db.heap(t);
        for (int i = 0; i < 3; i++) heap.insert(encode(t.cols, {intValue(40 + i), textValue("bob"), intValue(7)}));
        std::string got;
        for (uint32_t slot : {4294967295u, 2147483648u}) got += errorOf([&] { heap.get({1, slot}); }) + "\n" + errorOf([&] { heap.remove({1, slot}); }) + "\n";
        uint8_t* page = db.pool.fetch({t.file, 1});
        std::string header = ints({slotCount(page), freeEnd(page)});
        db.pool.unpin({t.file, 1}, false);
        std::string want = "error: no record at rid (1, 4294967295), the page has 3 slots\nerror: no record at rid (1, 4294967295), the page has 3 slots\n"
                           "error: no record at rid (1, 2147483648), the page has 3 slots\nerror: no record at rid (1, 2147483648), the page has 3 slots\n";
        return got == want && header == "3 4057 " && t.liveTuples == 3 ? "" : got + header;
    }},
    {"a record shorter than its schema is a corrupt record, not a read past its end", [] {
        std::vector<Col> varchar = {{"R", "b", StrT, 20}}, integer = {{"R", "a", IntT}};
        std::string got = errorOf([&] { decode(integer, std::string_view("ab", 2)); }) + "\n" + errorOf([&] { decode(varchar, std::string_view("\x05\x00" "ab", 4)); }) + "\n";
        std::string want = "error: corrupt record: 2 bytes are too few for its schema\nerror: corrupt record: 4 bytes are too few for its schema\n";
        return got == want ? "" : got;
    }},
    {"a record of 4084 bytes is stored and one of 4085 is an error from the loader and the heap file", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        std::string schema, fits, over;
        for (int i = 0; i < 16; i++) {
            schema += (i ? "," : "") + std::string("c") + std::to_string(i) + ":VARCHAR(255)";
            fits += (i ? "," : "") + std::string(i < 15 ? 255 : 227, 'x');
            over += (i ? "," : "") + std::string(i < 15 ? 255 : 228, 'x');
        }
        tool({"create-table", "--db", DIR, "--name", "Big", "--schema", schema});
        std::ofstream(DIR + "/fits.csv") << fits << "\n";
        std::ofstream(DIR + "/over.csv") << over << "\n";
        std::string got = tool({"load", "--db", DIR, "--table", "Big", "--file", DIR + "/fits.csv"}) + tool({"load", "--db", DIR, "--table", "Big", "--file", DIR + "/over.csv"});
        std::string free = tool({"layout", "--db", DIR, "--table", "Big", "--page", "1"});
        Database db(DIR, 8);
        Table& t = db.table("Big");
        HeapFile heap = db.heap(t);
        got += errorOf([&] { heap.insert(std::string(4085, 'x')); }) + "\n";
        std::string want = "loaded 1 rows into Big, now 1 tuples on 1 data pages\n"
                           "error: table Big, row 1: a record of 4085 bytes does not fit on a page (at most 4084)\n"
                           "error: table Big: a record of 4085 bytes does not fit on a page (at most 4084)\n";
        bool unchanged = t.dataPages == 1 && t.liveTuples == 1 && db.disk.numPages(t.file) == 2 && db.pool.held == 0;
        return got == want && free.find("free space 0 bytes") != std::string::npos && unchanged ? "" : got + free;
    }},
    {"an insert with no frame for its data page leaves nothing pinned", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT"});
        Database db(DIR, 1);
        HeapFile heap = db.heap(db.table("R"));
        std::string got = errorOf([&] { heap.insert(std::string(4, 'x')); });
        return got == "error: no unpinned frame available, all 1 frames are pinned" && db.pool.held == 0 ? "" : got + " held " + std::to_string(db.pool.held);
    }},
    {"a record longer than 4084 bytes names the table and row, and nothing is loaded", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        std::string schema, line;
        for (int i = 0; i < 16; i++) { schema += (i ? "," : "") + std::string("c") + std::to_string(i) + ":VARCHAR(255)"; line += (i ? "," : "") + std::string(255, 'x'); }
        std::ofstream(DIR + "/big.csv") << std::string(15, ',') << "\n" << line << "\n";
        tool({"create-table", "--db", DIR, "--name", "Big", "--schema", schema});
        std::string got = tool({"load", "--db", DIR, "--table", "Big", "--file", DIR + "/big.csv"});
        std::string after = tool({"scan", "--db", DIR, "--table", "Big"});
        bool ok = got == "error: table Big, row 2: a record of 4112 bytes does not fit on a page (at most 4084)\n" && after.find("\n0 rows,") != std::string::npos;
        return ok ? "" : got + after;
    }},
    {"bad CSV fields name the line and the column", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT,b:VARCHAR(3)"});
        std::string got;
        for (std::string text : {"1,ab\nx,ab\n", "1,abcd\n", "1\n", "99999999999,a\n", "2147483648,a\n", "-2147483649,a\n", "99999999999999999999999,a\n", "-2147483648,\"a,b\"\n"}) {
            std::ofstream(DIR + "/r.csv") << text;
            got += tool({"load", "--db", DIR, "--table", "R", "--file", DIR + "/r.csv"});
        }
        std::string want =
            "error: line 2, column 1 (a): 'x' is not an INT\n"
            "error: line 1, column 2 (b): 'abcd' is longer than VARCHAR(3)\n"
            "error: line 1 has 1 fields but table R has 2 columns\n"
            "error: line 1, column 1 (a): '99999999999' is not an INT\n"
            "error: line 1, column 1 (a): '2147483648' is not an INT\n"
            "error: line 1, column 1 (a): '-2147483649' is not an INT\n"
            "error: line 1, column 1 (a): '99999999999999999999999' is not an INT\n"
            "loaded 1 rows into R, now 1 tuples on 1 data pages\n";
        return got == want ? "" : got;
    }},
    {"an INT is read by value, however many leading zeros it has", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT"});
        std::ofstream(DIR + "/r.csv") << "000000000001\n-02147483648\n+0000000000000000000002147483647\n007\n";
        std::string got = tool({"load", "--db", DIR, "--table", "R", "--file", DIR + "/r.csv"}) + tool({"scan", "--db", DIR, "--table", "R"});
        std::string want = "loaded 4 rows into R, now 4 tuples on 1 data pages\na\n1\n-2147483648\n2147483647\n7\n4 rows, hits=0 misses=2 evictions=0 reads=2 writes=0\n";
        return got == want ? "" : got;
    }},
    {"a VARCHAR field that is not UTF-8 names the line, the column and the byte", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT,b:VARCHAR(8)"});
        std::string got;
        for (std::string field : {"a\xff" "b", "\xc0\xaf", "\xed\xa0\x80", "ok\xe2\x82", "\xf4\x90\x80\x80", "\x80"}) {
            std::ofstream(DIR + "/r.csv") << "1,ok\n2," << field << "\n";
            got += tool({"load", "--db", DIR, "--table", "R", "--file", DIR + "/r.csv"});
        }
        std::ofstream(DIR + "/r.csv") << "1,\xc3\xa9\xe2\x82\xac\n2,\xf0\x9f\x98\x80\n";
        got += tool({"load", "--db", DIR, "--table", "R", "--file", DIR + "/r.csv"});
        std::string want =
            "error: line 2, column 2 (b): byte 2 is not valid UTF-8\n"
            "error: line 2, column 2 (b): byte 1 is not valid UTF-8\n"
            "error: line 2, column 2 (b): byte 1 is not valid UTF-8\n"
            "error: line 2, column 2 (b): byte 3 is not valid UTF-8\n"
            "error: line 2, column 2 (b): byte 1 is not valid UTF-8\n"
            "error: line 2, column 2 (b): byte 1 is not valid UTF-8\n"
            "loaded 2 rows into R, now 2 tuples on 1 data pages\n";
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
    {"a table may not be called catalog in any case, and the catalog survives the attempt", [] {
        fresh();
        std::string got = tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT"});
        for (std::string name : {"catalog", "CATALOG", "Catalog"}) got += tool({"create-table", "--db", DIR, "--name", name, "--schema", "x:INT"});
        got += tool({"scan", "--db", DIR, "--table", "R"});
        std::string want = "created table R (a INT) in " + DIR + "/R.db\n"
                           "error: table name catalog is taken by the catalog's own file, catalog.db\n"
                           "error: table name CATALOG is taken by the catalog's own file, catalog.db\n"
                           "error: table name Catalog is taken by the catalog's own file, catalog.db\n"
                           "a\n0 rows, hits=0 misses=1 evictions=0 reads=1 writes=0\n";
        return got == want ? "" : got;
    }},
    {"a table name is an identifier short enough for a file name, so no file is made outside the database", [] {
        fresh();
        std::string got;
        for (std::string name : {"../victim", "", "a.b", "1a", "a b", "a/b"}) got += tool({"create-table", "--db", DIR, "--name", name, "--schema", "x:INT"});
        got += tool({"create-table", "--db", DIR, "--name", std::string(253, 'a'), "--schema", "x:INT"});
        std::string longest = tool({"create-table", "--db", DIR, "--name", "_" + std::string(251, 'a'), "--schema", "x:INT"});
        std::string rule = " must be a letter or underscore followed by letters, digits and underscores\n";
        std::string want = "error: table name '../victim'" + rule + "error: table name ''" + rule + "error: table name 'a.b'" + rule + "error: table name '1a'" + rule +
                           "error: table name 'a b'" + rule + "error: table name 'a/b'" + rule +
                           "error: table name " + std::string(253, 'a') + " is longer than 252 characters, the most that leaves room for .db in a file name\n";
        bool made = longest == "created table _" + std::string(251, 'a') + " (x INT) in " + DIR + "/_" + std::string(251, 'a') + ".db\n";
        return got == want && made && !std::filesystem::exists("tests/victim.db") && filesIn(DIR) == "_" + std::string(251, 'a') + ".db catalog.db " ? "" : got + filesIn(DIR);
    }},
    {"table names that differ only in case would share a file, so the second is refused", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        std::ofstream(DIR + "/d.csv") << "1,x\n2,y\n";
        tool({"create-table", "--db", DIR, "--name", "Dept", "--schema", "a:INT,b:VARCHAR(5)"});
        tool({"load", "--db", DIR, "--table", "Dept", "--file", DIR + "/d.csv"});
        std::string got = tool({"create-table", "--db", DIR, "--name", "dept", "--schema", "a:INT"}) + tool({"create-table", "--db", DIR, "--name", "DEPT", "--schema", "a:INT"}) +
                          tool({"scan", "--db", DIR, "--table", "Dept"});
        std::string want = "error: table dept would share a file with table Dept: table names may not differ only in case\n"
                           "error: table DEPT would share a file with table Dept: table names may not differ only in case\n"
                           "a, b\n1, x\n2, y\n2 rows, hits=0 misses=2 evictions=0 reads=2 writes=0\n";
        return got == want ? "" : got;
    }},
    {"create-table never overwrites a file that is already there", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT"});
        std::ofstream(DIR + "/S.db") << "not a table";
        std::string got = tool({"create-table", "--db", DIR, "--name", "S", "--schema", "a:INT"});
        std::string want = "io error: cannot create " + DIR + "/S.db: File exists\n";
        return got == want && readFile(DIR + "/S.db") == "not a table" && tool({"scan", "--db", DIR, "--table", "S"}) == "error: unknown table S\n" ? "" : got;
    }},
    {"a column name longer than 255 characters is refused, and one of 255 survives a restart", [] {
        fresh();
        std::string got = tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT," + std::string(256, 'n') + ":INT"});
        std::string table(252, 't'), column(255, 'n');
        tool({"create-table", "--db", DIR, "--name", table, "--schema", "a:INT," + column + ":VARCHAR(255)"});
        Database db(DIR, 8);
        bool back = db.tables.size() == 1 && db.table(table).cols.size() == 2 && db.table(table).cols[1].name == column && db.table(table).cols[1].length == 255;
        return got == "error: column name " + std::string(256, 'n') + " is longer than 255 characters\n" && back ? "" : got;
    }},
    {"--frames 0 is one error for every command and both policies", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        std::ofstream(DIR + "/t.txt") << "1\n";
        std::ofstream(DIR + "/r.csv") << "1\n";
        tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT"});
        std::string got = tool({"scan", "--db", DIR, "--table", "R", "--frames", "0", "--policy", "clock"}) + tool({"scan", "--db", DIR, "--table", "R", "--frames", "0", "--policy", "lru"}) +
                          tool({"load", "--db", DIR, "--table", "R", "--file", DIR + "/r.csv", "--frames", "0"}) + tool({"layout", "--db", DIR, "--table", "R", "--page", "0", "--frames", "0"}) +
                          tool({"create-table", "--db", DIR, "--name", "S", "--schema", "a:INT", "--frames", "0"}) +
                          tool({"trace", "--db", DIR + "/scratch", "--file", DIR + "/t.txt", "--frames", "0", "--policy", "clock"});
        std::string once = "error: --frames must be at least 1\n";
        return got == once + once + once + once + once + once && !std::filesystem::exists(DIR + "/scratch") ? "" : got;
    }},
    {"create-table and layout take --frames", [] {
        fresh();
        std::string got = tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT", "--frames", "2"}) + tool({"layout", "--db", DIR, "--table", "R", "--page", "0", "--frames", "1"});
        std::string want = "created table R (a INT) in " + DIR + "/R.db\nR.db page 0, directory page\noffset  size  field\n     0     4  page_id        0\n"
                           "     4     4  next_dir_page  none (0xFFFFFFFF)\n     8     2  entry_count    0\nentry  at offset  data_page  free_bytes\n";
        return got == want ? "" : got;
    }},
    {"scan, layout and load do not make a database where there is none", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        std::ofstream(DIR + "/r.csv") << "1\n";
        std::string none = DIR + "/none";
        std::string got = tool({"scan", "--db", none, "--table", "R"}) + tool({"layout", "--db", none, "--table", "R", "--page", "0"}) + tool({"load", "--db", none, "--table", "R", "--file", DIR + "/r.csv"});
        std::string once = "error: " + none + " is not a database, it has no catalog.db\n";
        return got == once + once + once && !std::filesystem::exists(none) ? "" : got;
    }},
    {"a missing or empty table file, a zero page and an empty catalog are errors, not hangs", [] {
        fresh();
        tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT"});
        std::filesystem::remove(DIR + "/R.db");
        std::string got = tool({"scan", "--db", DIR, "--table", "R"});
        bool recreated = std::filesystem::exists(DIR + "/R.db");
        std::ofstream(DIR + "/R.db").close();
        got += tool({"scan", "--db", DIR, "--table", "R"});
        std::ofstream(DIR + "/R.db") << std::string(PAGE_SIZE, '\0');
        got += tool({"scan", "--db", DIR, "--table", "R"}) + tool({"layout", "--db", DIR, "--table", "R", "--page", "0"});
        std::filesystem::create_directories(DIR + "/empty");
        std::ofstream(DIR + "/empty/catalog.db").close();
        got += tool({"scan", "--db", DIR + "/empty", "--table", "R"});
        std::string corrupt = "error: corrupt table file " + DIR + "/R.db: directory page 0 links back to page 0\n";
        std::string want = "io error: cannot open " + DIR + "/R.db: No such file or directory\n"
                           "io error: " + DIR + "/R.db has 0 pages, there is no page 0\n" + corrupt + corrupt +
                           "io error: " + DIR + "/empty/catalog.db has 0 pages, there is no page 0\n";
        return got == want && !recreated ? "" : got;
    }},
    {"a catalog save never rewrites catalog.db in place, and a failed one leaves the old catalog and no files", [] {
        fresh();
        tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:INT"});
        std::string before = readFile(DIR + "/catalog.db");
        std::ifstream old(DIR + "/catalog.db", std::ios::binary);
        tool({"create-table", "--db", DIR, "--name", "S", "--schema", "a:INT"});
        std::string stillOld(std::istreambuf_iterator<char>(old), {});
        std::string failed = tool({"create-table", "--db", DIR, "--name", "T", "--schema", "a:INT", "--frames", "1"});
        std::string after = tool({"scan", "--db", DIR, "--table", "S"}) + tool({"scan", "--db", DIR, "--table", "T"});
        bool ok = stillOld == before && readFile(DIR + "/catalog.db") != before && failed == "error: no unpinned frame available, all 1 frames are pinned\n" &&
                  after == "a\n0 rows, hits=0 misses=1 evictions=0 reads=1 writes=0\nerror: unknown table T\n" && filesIn(DIR) == "R.db S.db catalog.db ";
        return ok ? "" : failed + after + filesIn(DIR);
    }},
    {"a bad --db, a long name or a huge VARCHAR length is a one line error, not an abort", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        std::ofstream(DIR + "/afile") << "x";
        std::string got = tool({"scan", "--db", DIR + "/afile", "--table", "R"}) + tool({"create-table", "--db", DIR + "/afile", "--name", "R", "--schema", "a:INT"}) +
                          tool({"create-table", "--db", DIR, "--name", std::string(300, 'a'), "--schema", "a:INT"}) +
                          tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:VARCHAR(99999999999)"}) +
                          tool({"create-table", "--db", DIR, "--name", "R", "--schema", "a:VARCHAR(0000000000000000000255)"});
        std::string want = "error: " + DIR + "/afile is not a database, it has no catalog.db\n"
                           "io error: cannot create directory " + DIR + "/afile: File exists\n"
                           "error: table name " + std::string(300, 'a') + " is longer than 252 characters, the most that leaves room for .db in a file name\n"
                           "error: bad schema \"a:VARCHAR(99999999999)\": VARCHAR length must be 1 to 255 at character 22\n"
                           "created table R (a VARCHAR(255)) in " + DIR + "/R.db\n";
        return got == want ? "" : got;
    }},
    {"an exception that is not an Error is reported on one line", [] {
        fresh();
        {
            Database db(DIR, 8);
            Table& t = db.createTable("R", parseSchema("a:INT"));
            t.stats.analyzed = true;
            t.stats.columns[0] = {1, textValue("abc"), textValue("abc"), {}};
            db.saveCatalog();
        }
        std::ostringstream out;
        int code = storageTool({"scan", "--db", DIR, "--table", "R"}, out);
        return code == 1 && out.str() == "internal error: stoll: no conversion\n" ? "" : out.str();
    }},
    {"a scratch file is deleted however its scope is left", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        DiskManager disk;
        std::string got = errorOf([&] { TempFile scratch(disk, DIR); throw Error{"error: interrupted"}; });
        return got == "error: interrupted" && filesIn(DIR).empty() ? "" : filesIn(DIR);
    }},
    {"CLOCK is the default policy", [] {
        fresh();
        std::filesystem::create_directories(DIR);
        std::ofstream(DIR + "/t.txt") << "1\n2\n3\n1\n4\n2\n";
        std::string got = tool({"trace", "--db", DIR, "--file", DIR + "/t.txt", "--frames", "3"});
        Database db(DIR + "/db", 8);
        return got == "policy=clock frames=3 hits=2 misses=4 evictions=1 reads=4 writes=0 hit_rate=0.3333\n" && db.pool.policy == Clock ? "" : got;
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
