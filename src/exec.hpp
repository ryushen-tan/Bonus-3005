// Module 2: iterator operators that run inside a budget of B buffer pool frames
#pragma once
#include "catalog.hpp"
#include <deque>
#include <memory>

struct Counters { unsigned long long tuplesOut = 0, comparisons = 0; IoCounters io; };

// every disk call made while an operator's method runs is charged to that operator
struct Charge {
    DiskManager& disk;
    IoCounters* saved;
    Charge(DiskManager& d, IoCounters& c) : disk(d), saved(d.attributed) { d.attributed = &c; }
    ~Charge() { disk.attributed = saved; }
};

struct Operator {
    Database& db;
    std::string op, detail;
    std::vector<Col> schema;
    std::vector<std::unique_ptr<Operator>> children;
    // set for the inputs of an operator that holds memory when that input holds memory too
    std::vector<bool> materialize;
    Counters counters;
    std::string notes;
    // frames this operator must leave free: one while its parent is yet to take the frame it writes this
    // operator's output through, zero once it has
    int parentFrames = 0;

    Operator(Database& d, std::string name) : db(d), op(std::move(name)) {}
    virtual ~Operator() = default;
    void open() { Charge c(db.disk, counters.io); start(); }
    bool next(Row& row) {
        Charge c(db.disk, counters.io);
        if (!produce(row)) return false;
        counters.tuplesOut++;
        return true;
    }
    void close() { Charge c(db.disk, counters.io); finish(); }
    virtual void start() {}
    virtual bool produce(Row& row) = 0;
    virtual void finish() {}
    virtual bool holdsMemory() const { return false; }
    int available() const { return db.pool.freeFrames() - parentFrames; }
};

// frames taken from the pool as an operator's working memory
struct Frames {
    BufferPool& pool;
    std::vector<size_t> held;
    explicit Frames(BufferPool& p) : pool(p) {}
    Frames(const Frames&) = delete;
    ~Frames() { releaseAll(); }
    void take(int n) { for (int i = 0; i < n; i++) held.push_back(pool.reserve()); }
    void releaseAll() { for (size_t f : held) pool.release(f); held.clear(); }
    int count() const { return int(held.size()); }
};

// the pages a sequence of records fills when packed one after another into slotted pages
struct Packing {
    int pages = 0;
    size_t room = 0;
    bool add(size_t length, int limit) {
        size_t need = length + SLOT_SIZE;
        if (pages > 0 && room >= need) { room -= need; return true; }
        if (pages == limit) return false;
        pages++;
        room = need > size_t(MAX_RECORD) ? 0 : MAX_RECORD - need;
        return true;
    }
};

// an operator's own temporary file, deleted when the operator is done with it
struct SpillFile {
    DiskManager& disk;
    uint32_t file;
    explicit SpillFile(Database& db) : disk(db.disk), file(db.disk.createTemp(db.dir)) {}
    SpillFile(const SpillFile&) = delete;
    ~SpillFile() { disk.remove(file); }
};
struct Run { uint32_t file = 0; std::vector<uint32_t> pages; long long tuples = 0; };

// writes tuples page by page through one frame, each full page straight to disk
struct RunWriter {
    Database& db;
    const std::vector<Col>& schema;
    size_t frame;
    bool ownsFrame;
    Run run;
    RunWriter(Database& d, uint32_t file, const std::vector<Col>& s) : RunWriter(d, file, s, d.pool.reserve(), true) {}
    RunWriter(Database& d, uint32_t file, const std::vector<Col>& s, size_t f, bool owns) : db(d), schema(s), frame(f), ownsFrame(owns) {
        run.file = file;
        initDataPage(page(), 0);
    }
    RunWriter(const RunWriter&) = delete;
    ~RunWriter() { if (ownsFrame) db.pool.release(frame); }
    uint8_t* page() { return db.pool.memory(frame); }
    void add(const Row& row) {
        std::string record = encode(schema, row);
        if (record.size() > size_t(MAX_RECORD)) throw Error{"error: a tuple of " + std::to_string(record.size()) + " bytes does not fit on a page of a temporary file"};
        if (insertRecord(page(), record) == NO_SPACE) { flush(); insertRecord(page(), record); }
        run.tuples++;
    }
    void flush() {
        if (slotCount(page()) == 0) return;
        uint32_t pageNo = db.disk.allocatePage(run.file);
        put32(page(), pageNo);
        db.disk.writePage({run.file, pageNo}, page());
        run.pages.push_back(pageNo);
        initDataPage(page(), 0);
    }
    Run finish() { flush(); return std::move(run); }
};

// reads a run page by page into one frame; a position can be remembered and gone back to. Given a
// second frame it keeps the page it last moved off, so going back one page costs no read
struct RunReader {
    struct Position { size_t page; int slot; };
    Database& db;
    const std::vector<Col>& schema;
    size_t frame, spare;
    bool hasSpare;
    const Run* run = nullptr;
    size_t loaded = SIZE_MAX, spareLoaded = SIZE_MAX;
    Position at{0, 0};
    RunReader(Database& d, const std::vector<Col>& s, bool keepPrevious = false)
        : db(d), schema(s), frame(d.pool.reserve()), spare(keepPrevious ? d.pool.reserve() : 0), hasSpare(keepPrevious) {}
    RunReader(const RunReader&) = delete;
    ~RunReader() { db.pool.release(frame); if (hasSpare) db.pool.release(spare); }
    void start(const Run& r) { run = &r; at = {0, 0}; loaded = spareLoaded = SIZE_MAX; }
    void load(size_t page) {
        if (loaded == page) return;
        if (hasSpare) {
            std::swap(frame, spare);
            std::swap(loaded, spareLoaded);
            if (loaded == page) return;
        }
        db.disk.readPage({run->file, run->pages[page]}, db.pool.memory(frame));
        loaded = page;
    }
    // where the record the last next() returned is
    Position last() const { return {at.page, at.slot - 1}; }
    void seek(Position p) { at = p; }
    bool next(Row& row) {
        for (; at.page < run->pages.size(); at = {at.page + 1, 0}) {
            load(at.page);
            const uint8_t* page = db.pool.memory(frame);
            while (at.slot < slotCount(page)) {
                int s = at.slot++;
                if (live(page, s)) { row = decode(schema, recordAt(page, s)); return true; }
            }
        }
        return false;
    }
};

// where an operator's input tuples come from: a child operator, or a run the child was written to
struct TupleSource {
    virtual ~TupleSource() = default;
    virtual bool next(Row& row) = 0;
    virtual void rewind() = 0;
    virtual void close() = 0;
    // frames the source still has to pin when it is read; a run's reader already holds its frame
    virtual int framesToCome() const = 0;
};
struct ChildSource : TupleSource {
    Operator& child;
    bool isOpen = false;
    explicit ChildSource(Operator& c) : child(c) {}
    ~ChildSource() override { close(); }
    bool next(Row& row) override { if (!isOpen) rewind(); return child.next(row); }
    void rewind() override { close(); child.open(); isOpen = true; }
    void close() override { if (isOpen) { child.close(); isOpen = false; } }
    int framesToCome() const override { return 1; }
};
struct RunSource : TupleSource {
    Run run;
    RunReader reader;
    RunSource(Database& db, Run r, const std::vector<Col>& schema) : run(std::move(r)), reader(db, schema) { reader.start(run); }
    bool next(Row& row) override { return reader.next(row); }
    void rewind() override { reader.start(run); }
    void close() override {}
    int framesToCome() const override { return 0; }
};

// for a pipelined operator: open, next, close of its only child
inline void openChild(Operator& op) { op.children[0]->open(); }

// an input that holds memory is run to completion into a temporary file before its parent takes any
// memory of its own, so no two operators ever hold working memory at the same time
inline void parentFrameTaken(Operator& op) {
    op.parentFrames = 0;
    if (!op.holdsMemory()) for (auto& c : op.children) parentFrameTaken(*c);
}
// the child is opened before the output frame is taken, so whatever it materialises itself runs with that
// frame still free, and only one output frame is ever held for any depth of materialisation
inline Run materialized(Operator& op, size_t i, std::unique_ptr<SpillFile>& spill) {
    Operator& child = *op.children[i];
    if (!spill) spill = std::make_unique<SpillFile>(op.db);
    child.open();
    RunWriter writer(op.db, spill->file, child.schema);
    parentFrameTaken(child);
    Row row;
    while (child.next(row)) writer.add(row);
    child.close();
    return writer.finish();
}
inline std::unique_ptr<TupleSource> inputOf(Operator& op, size_t i, std::unique_ptr<SpillFile>& spill) {
    if (!op.materialize[i]) return std::make_unique<ChildSource>(*op.children[i]);
    return std::make_unique<RunSource>(op.db, materialized(op, i, spill), op.children[i]->schema);
}

struct SortKey { size_t index; bool desc; };
inline int compareKeys(const Row& a, const Row& b, const std::vector<SortKey>& keys) {
    for (const SortKey& k : keys) if (int d = compare(a[k.index], b[k.index])) return k.desc ? -d : d;
    return 0;
}
inline std::vector<SortKey> allColumns(size_t n) { std::vector<SortKey> keys; for (size_t i = 0; i < n; i++) keys.push_back({i, false}); return keys; }

// a stable bottom-up merge sort for one run in memory; written here rather than std::sort so the number
// of comparisons is the same on every platform
template <class Less> void mergeSort(std::vector<Row>& rows, Less less) {
    std::vector<Row> buffer(rows.size());
    for (size_t width = 1; width < rows.size(); width *= 2) {
        for (size_t lo = 0; lo < rows.size(); lo += 2 * width) {
            size_t mid = std::min(lo + width, rows.size()), hi = std::min(lo + 2 * width, rows.size()), i = lo, j = mid, k = lo;
            while (i < mid && j < hi) buffer[k++] = less(rows[j], rows[i]) ? std::move(rows[j++]) : std::move(rows[i++]);
            while (i < mid) buffer[k++] = std::move(rows[i++]);
            while (j < hi) buffer[k++] = std::move(rows[j++]);
        }
        std::swap(rows, buffer);
    }
}

// external merge sort: pass 0 writes sorted runs, merge passes combine B - 1 runs at a time through one
// output frame, and the final pass streams to the caller instead of writing
struct ExternalSort {
    Database& db;
    const std::vector<Col>& schema;
    std::vector<SortKey> keys;
    unsigned long long& comparisons;
    int keep;
    std::unique_ptr<SpillFile> spill;
    std::vector<Run> runs;
    std::vector<Row> memory;
    size_t memoryAt = 0;
    Frames frames;
    int passes = 0;
    std::vector<std::unique_ptr<RunReader>> readers;
    std::vector<Row> heads;
    std::vector<bool> live;

    ExternalSort(Database& d, const std::vector<Col>& s, std::vector<SortKey> k, unsigned long long& c, int keepFree = 0)
        : db(d), schema(s), keys(std::move(k)), comparisons(c), keep(keepFree), frames(d.pool) {}
    int cmp(const Row& a, const Row& b) { comparisons++; return compareKeys(a, b, keys); }
    uint32_t spillFile() { if (!spill) spill = std::make_unique<SpillFile>(db); return spill->file; }
    void sortMemory() { mergeSort(memory, [&](const Row& a, const Row& b) { return cmp(a, b) < 0; }); }
    void writeMemory() {
        sortMemory();
        RunWriter writer(db, spillFile(), schema, frames.held[0], false);
        for (const Row& r : memory) writer.add(r);
        runs.push_back(writer.finish());
        memory.clear();
    }
    // pass 0. A run is B pages: the frames reserved here and the frame the input page arrives in, the
    // textbook arrangement where the input pages are read into the sort's own buffers
    void generate(TupleSource& in) {
        int work = db.pool.freeFrames() - keep - in.framesToCome();
        if (work < 1) throw Error{"error: sort needs at least 2 frames for its runs, " + std::to_string(work + in.framesToCome()) + " are free"};
        frames.take(work);
        passes = 1;
        Packing pack;
        Row row;
        while (in.next(row)) {
            size_t length = encodedSize(schema, row);
            if (!pack.add(length, work + 1)) { writeMemory(); pack = {}; pack.add(length, work + 1); }
            memory.push_back(std::move(row));
        }
        in.close();
        if (runs.empty()) sortMemory();
        else writeMemory();
        if (!runs.empty()) frames.releaseAll();
    }
    // merges groups of B - 1 runs, carrying a group of one forward untouched, until at most B - 1 runs are
    // left for the final pass, or until there is a single run when the caller needs one on disk
    void merge(bool toOneRun) {
        if (runs.empty()) {
            if (!toOneRun) return;
            writeMemory();
            frames.releaseAll();
            return;
        }
        int fanIn = db.pool.freeFrames() - keep - 1;
        if (fanIn < 2) throw Error{"error: sort needs at least 3 frames to merge, " + std::to_string(fanIn + 1) + " are free"};
        while (runs.size() > (toOneRun ? 1 : size_t(fanIn))) {
            passes++;
            std::vector<Run> merged;
            for (size_t i = 0; i < runs.size(); i += size_t(fanIn)) {
                size_t end = std::min(runs.size(), i + size_t(fanIn));
                if (end - i == 1) { merged.push_back(std::move(runs[i])); continue; }
                RunWriter writer(db, spillFile(), schema);
                startMerge(i, end);
                Row row;
                while (nextMerged(row)) writer.add(row);
                readers.clear();
                merged.push_back(writer.finish());
            }
            runs = std::move(merged);
        }
    }
    void startMerge(size_t from, size_t to) {
        readers.clear(); heads.assign(to - from, {}); live.assign(to - from, false);
        for (size_t i = from; i < to; i++) {
            readers.push_back(std::make_unique<RunReader>(db, schema));
            readers.back()->start(runs[i]);
            live[i - from] = readers.back()->next(heads[i - from]);
        }
    }
    bool nextMerged(Row& out) {
        size_t best = SIZE_MAX;
        for (size_t i = 0; i < heads.size(); i++)
            if (live[i] && (best == SIZE_MAX || cmp(heads[i], heads[best]) < 0)) best = i;
        if (best == SIZE_MAX) return false;
        out = std::move(heads[best]);
        live[best] = readers[best]->next(heads[best]);
        return true;
    }
    // the final pass, from memory when the input fitted in one run, otherwise merging what is left
    void startFinal() {
        if (runs.empty()) { memoryAt = 0; return; }
        passes++;
        startMerge(0, runs.size());
    }
    bool next(Row& out) {
        if (!runs.empty()) return nextMerged(out);
        if (memoryAt == memory.size()) return false;
        out = std::move(memory[memoryAt++]);
        return true;
    }
    Run single() { merge(true); return std::move(runs[0]); }
    void close() { readers.clear(); frames.releaseAll(); memory.clear(); runs.clear(); spill.reset(); }
};

// a sequence of FNV-1a hashes over a value's bytes; numbers hash by value so 1 and 1.0 agree
inline uint64_t hashValue(const Value& v, uint64_t h) {
    auto mix = [&](const void* data, size_t n) { for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t*)data)[i]; h *= 1099511628211ULL; } };
    if (v.isNum) { double d = v.num == 0 ? 0 : v.num; mix(&d, 8); }
    else mix(v.text.data(), v.text.size());
    return h;
}
inline uint64_t hashKey(const Row& row, const std::vector<size_t>& cols, uint64_t seed) {
    uint64_t h = 14695981039346656037ULL ^ (seed * 0x9E3779B97F4A7C15ULL);
    for (size_t c : cols) h = hashValue(row[c], h);
    return h;
}
inline bool sameKey(const Row& a, const std::vector<size_t>& ka, const Row& b, const std::vector<size_t>& kb) {
    for (size_t i = 0; i < ka.size(); i++) if (compare(a[ka[i]], b[kb[i]]) != 0) return false;
    return true;
}
inline Row joined(const Row& a, const Row& b) { Row r = a; r.insert(r.end(), b.begin(), b.end()); return r; }

// ---- the operators ----

struct SeqScan : Operator {
    Table& table;
    std::unique_ptr<HeapScan> scan;
    SeqScan(Database& d, Table& t) : Operator(d, "seqscan"), table(t) { detail = t.name; schema = t.cols; }
    void start() override { scan = std::make_unique<HeapScan>(db.pool, table.file); }
    bool produce(Row& row) override {
        Rid rid;
        std::string_view record;
        if (!scan->next(rid, record)) return false;
        row = decode(table.cols, record);
        return true;
    }
    void finish() override { scan.reset(); }
};

struct Filter : Operator {
    Node cond;
    Filter(Database& d, Node c) : Operator(d, "filter"), cond(std::move(c)) {}
    void start() override { openChild(*this); }
    bool produce(Row& row) override {
        while (children[0]->next(row)) { counters.comparisons++; if (eval(cond, row)) return true; }
        return false;
    }
    void finish() override { children[0]->close(); }
};

struct Projection : Operator {
    std::vector<size_t> columns;
    Projection(Database& d) : Operator(d, "project") {}
    void start() override { openChild(*this); }
    bool produce(Row& row) override {
        Row in;
        if (!children[0]->next(in)) return false;
        row.clear();
        for (size_t c : columns) row.push_back(std::move(in[c]));
        return true;
    }
    void finish() override { children[0]->close(); }
};

struct Renaming : Operator {
    Renaming(Database& d) : Operator(d, "rename") {}
    void start() override { openChild(*this); }
    bool produce(Row& row) override { return children[0]->next(row); }
    void finish() override { children[0]->close(); }
};

struct Limit : Operator {
    long long n, given = 0;
    Limit(Database& d, long long count) : Operator(d, "limit"), n(count) { detail = std::to_string(count); }
    void start() override { given = 0; openChild(*this); }
    bool produce(Row& row) override {
        if (given == n || !children[0]->next(row)) return false;
        given++;
        return true;
    }
    void finish() override { children[0]->close(); }
};

// the right input is opened only when the left is used up, so the two never hold frames at once
struct UnionAll : Operator {
    size_t side = 0;
    UnionAll(Database& d) : Operator(d, "union_all") {}
    void start() override { side = 0; children[0]->open(); }
    bool produce(Row& row) override {
        if (children[side]->next(row)) return true;
        if (side == 1) return false;
        children[0]->close();
        side = 1;
        children[1]->open();
        return children[1]->next(row);
    }
    void finish() override { children[side]->close(); }
};

struct Sort : Operator {
    std::vector<SortKey> keys;
    std::unique_ptr<ExternalSort> sorter;
    std::unique_ptr<SpillFile> spill;
    Sort(Database& d) : Operator(d, "sort") {}
    bool holdsMemory() const override { return true; }
    void start() override {
        std::unique_ptr<TupleSource> in = inputOf(*this, 0, spill);
        sorter = std::make_unique<ExternalSort>(db, schema, keys, counters.comparisons, parentFrames);
        sorter->generate(*in);
        in.reset();
        spill.reset();
        sorter->merge(false);
        sorter->startFinal();
        notes = "passes=" + std::to_string(sorter->passes);
    }
    bool produce(Row& row) override { return sorter->next(row); }
    void finish() override { sorter.reset(); }
};

// both sort and hash modes of distinct and group_by share the aggregate arithmetic
struct Aggregate { std::string func; long long column; std::string name; };
struct AggState { long long count = 0, sum = 0; Value min, max; };
inline void accumulate(std::vector<AggState>& states, const std::vector<Aggregate>& aggs, const Row& row) {
    for (size_t i = 0; i < aggs.size(); i++) {
        AggState& s = states[i];
        s.count++;
        if (aggs[i].column < 0) continue;
        const Value& v = row[size_t(aggs[i].column)];
        if (aggs[i].func == "sum" || aggs[i].func == "avg") s.sum += (long long)v.num;
        if (s.count == 1 || compare(v, s.min) < 0) s.min = v;
        if (s.count == 1 || compare(v, s.max) > 0) s.max = v;
    }
}
inline Value aggregateResult(const AggState& s, const Aggregate& a) {
    if (a.func == "count") return intValue(s.count);
    if (s.count == 0) throw Error{"error: " + a.func + " of an empty input with no grouping columns is undefined"};
    if (a.func == "sum") return intValue(s.sum);
    if (a.func == "avg") return realValue(double(s.sum) / double(s.count));
    return a.func == "min" ? s.min : s.max;
}

struct DistinctBase : Operator {
    bool hash;
    DistinctBase(Database& d, bool hashMode) : Operator(d, "distinct"), hash(hashMode) { detail = hashMode ? "hash" : "sort"; }
    bool holdsMemory() const override { return true; }
};

// sort mode: equal tuples end up next to each other
struct SortDistinct : DistinctBase {
    std::unique_ptr<ExternalSort> sorter;
    std::unique_ptr<SpillFile> spill;
    Row last;
    bool any = false;
    SortDistinct(Database& d) : DistinctBase(d, false) {}
    void start() override {
        std::unique_ptr<TupleSource> in = inputOf(*this, 0, spill);
        sorter = std::make_unique<ExternalSort>(db, schema, allColumns(schema.size()), counters.comparisons, parentFrames);
        sorter->generate(*in);
        in.reset();
        spill.reset();
        sorter->merge(false);
        sorter->startFinal();
        any = false;
    }
    bool produce(Row& row) override {
        while (sorter->next(row)) {
            if (any) { counters.comparisons++; if (same(row, last)) continue; }
            last = row;
            any = true;
            return true;
        }
        return false;
    }
    void finish() override { sorter.reset(); }
};

struct SortGroupBy : Operator {
    std::vector<size_t> keyColumns;
    std::vector<Aggregate> aggs;
    std::unique_ptr<ExternalSort> sorter;
    std::unique_ptr<TupleSource> plain;
    std::unique_ptr<SpillFile> spill;
    Row pending;
    bool havePending = false, emittedEmpty = false;
    SortGroupBy(Database& d) : Operator(d, "group_by") { detail = "sort"; }
    bool holdsMemory() const override { return true; }
    void start() override {
        std::unique_ptr<TupleSource> in = inputOf(*this, 0, spill);
        emittedEmpty = false;
        if (keyColumns.empty()) { plain = std::move(in); havePending = plain->next(pending); return; }
        std::vector<SortKey> keys;
        for (size_t c : keyColumns) keys.push_back({c, false});
        sorter = std::make_unique<ExternalSort>(db, children[0]->schema, keys, counters.comparisons, parentFrames);
        sorter->generate(*in);
        in.reset();
        spill.reset();
        sorter->merge(false);
        sorter->startFinal();
        havePending = sorter->next(pending);
    }
    bool pull(Row& row) { return plain ? plain->next(row) : sorter->next(row); }
    bool produce(Row& row) override {
        if (!havePending) {
            if (!keyColumns.empty() || emittedEmpty) return false;
            emittedEmpty = true;
            row.clear();
            std::vector<AggState> states(aggs.size());
            for (size_t i = 0; i < aggs.size(); i++) row.push_back(aggregateResult(states[i], aggs[i]));
            return true;
        }
        std::vector<AggState> states(aggs.size());
        Row group = pending;
        accumulate(states, aggs, pending);
        while ((havePending = pull(pending))) {
            if (!keyColumns.empty()) { counters.comparisons++; if (!sameKey(pending, keyColumns, group, keyColumns)) break; }
            accumulate(states, aggs, pending);
        }
        emittedEmpty = true;
        row.clear();
        for (size_t c : keyColumns) row.push_back(group[c]);
        for (size_t i = 0; i < aggs.size(); i++) row.push_back(aggregateResult(states[i], aggs[i]));
        return true;
    }
    void finish() override { sorter.reset(); plain.reset(); }
};

// hash mode of distinct and group_by: groups go into an in-memory table while it has room; a tuple whose
// group is not in a full table is written to one of P partitions instead, and each partition is then
// handled the same way with a different hash function
struct HashGrouping {
    Database& db;
    const std::vector<Col>& inputSchema;
    std::vector<size_t> keyColumns;
    const std::vector<Aggregate>& aggs;
    unsigned long long& comparisons;
    std::unique_ptr<SpillFile> spill;
    Frames table;
    int partitions = 0, tablePages = 0;
    struct Entry { Row row; std::vector<AggState> states; };
    std::vector<Entry> entries;
    std::vector<std::vector<int>> buckets;
    std::deque<std::pair<Run, int>> pending;
    std::vector<std::string> sizes;

    HashGrouping(Database& d, const std::vector<Col>& s, std::vector<size_t> keys, const std::vector<Aggregate>& a, unsigned long long& c)
        : db(d), inputSchema(s), keyColumns(std::move(keys)), aggs(a), comparisons(c), table(d.pool) {}

    // the frames the table and the partition buffers get, out of what is free beside the input page
    void plan(int work) {
        if (work < 2) throw Error{"error: hash grouping needs at least 3 frames, " + std::to_string(work + 1) + " are free"};
        partitions = std::max(1, work / 2);
        tablePages = work - partitions;
        table.take(tablePages);
    }
    size_t entryBytes(const Row& row) {
        size_t n = 16 * aggs.size();
        for (size_t c : keyColumns) n += inputSchema[c].type == StrT ? 2 + row[c].text.size() : 8;
        return n;
    }
    // one pass over an input at a hash level: fills the table, spills the rest to new partitions
    void consume(TupleSource& in, int level) {
        entries.clear();
        buckets.assign(size_t(tablePages) * 64, {});
        std::vector<std::unique_ptr<RunWriter>> writers;
        Packing pack;
        Row row;
        while (in.next(row)) {
            uint64_t h = hashKey(row, keyColumns, uint64_t(2 * level + 1));
            std::vector<int>& bucket = buckets[h % buckets.size()];
            int found = -1;
            for (int e : bucket) { comparisons++; if (sameKey(entries[size_t(e)].row, keyColumns, row, keyColumns)) { found = e; break; } }
            if (found < 0 && pack.add(entryBytes(row), tablePages)) {
                found = int(entries.size());
                bucket.push_back(found);
                entries.push_back({row, std::vector<AggState>(aggs.size())});
            }
            if (found >= 0) { accumulate(entries[size_t(found)].states, aggs, row); continue; }
            if (writers.empty()) {
                if (!spill) spill = std::make_unique<SpillFile>(db);
                for (int p = 0; p < partitions; p++) writers.push_back(std::make_unique<RunWriter>(db, spill->file, inputSchema));
            }
            writers[hashKey(row, keyColumns, uint64_t(2 * level + 2)) % size_t(partitions)]->add(row);
        }
        for (auto& w : writers) {
            Run r = w->finish();
            sizes.push_back(std::to_string(r.pages.size()));
            if (r.tuples) pending.emplace_back(std::move(r), level + 1);
        }
    }
    // the next partition into the table, false when there is none left
    bool nextPartition() {
        if (pending.empty()) return false;
        auto [run, level] = std::move(pending.front());
        pending.pop_front();
        RunSource in(db, std::move(run), inputSchema);
        consume(in, level);
        return true;
    }
    void close() { entries.clear(); buckets.clear(); pending.clear(); table.releaseAll(); spill.reset(); }
};

struct HashDistinct : DistinctBase {
    std::unique_ptr<HashGrouping> grouping;
    std::unique_ptr<SpillFile> spill;
    std::vector<Aggregate> none;
    size_t at = 0;
    HashDistinct(Database& d) : DistinctBase(d, true) {}
    void start() override {
        std::unique_ptr<TupleSource> in = inputOf(*this, 0, spill);
        std::vector<size_t> all;
        for (size_t i = 0; i < schema.size(); i++) all.push_back(i);
        grouping = std::make_unique<HashGrouping>(db, schema, all, none, counters.comparisons);
        grouping->plan(available() - in->framesToCome());
        grouping->consume(*in, 0);
        in.reset();
        spill.reset();
        at = 0;
    }
    bool produce(Row& row) override {
        while (at == grouping->entries.size()) { if (!grouping->nextPartition()) return false; at = 0; }
        row = grouping->entries[at++].row;
        return true;
    }
    void finish() override { notes = "partitions=" + std::to_string(grouping->partitions) + " spilled_pages=[" + join(grouping->sizes) + "]"; grouping.reset(); }
    static std::string join(const std::vector<std::string>& xs) { std::string s; for (size_t i = 0; i < xs.size(); i++) s += (i ? "," : "") + xs[i]; return s; }
};

struct HashGroupBy : Operator {
    std::vector<size_t> keyColumns;
    std::vector<Aggregate> aggs;
    std::unique_ptr<HashGrouping> grouping;
    std::unique_ptr<SpillFile> spill;
    size_t at = 0;
    bool emittedEmpty = false;
    HashGroupBy(Database& d) : Operator(d, "group_by") { detail = "hash"; }
    bool holdsMemory() const override { return true; }
    void start() override {
        std::unique_ptr<TupleSource> in = inputOf(*this, 0, spill);
        grouping = std::make_unique<HashGrouping>(db, children[0]->schema, keyColumns, aggs, counters.comparisons);
        grouping->plan(available() - in->framesToCome());
        grouping->consume(*in, 0);
        in.reset();
        spill.reset();
        at = 0;
        emittedEmpty = false;
    }
    bool produce(Row& row) override {
        while (at == grouping->entries.size()) {
            if (grouping->nextPartition()) { at = 0; continue; }
            if (!keyColumns.empty() || emittedEmpty) return false;
            emittedEmpty = true;
            row.clear();
            for (const Aggregate& a : aggs) row.push_back(aggregateResult({}, a));
            return true;
        }
        const HashGrouping::Entry& e = grouping->entries[at++];
        emittedEmpty = true;
        row.clear();
        for (size_t c : keyColumns) row.push_back(e.row[c]);
        for (size_t i = 0; i < aggs.size(); i++) row.push_back(aggregateResult(e.states[i], aggs[i]));
        return true;
    }
    void finish() override { notes = "partitions=" + std::to_string(grouping->partitions) + " spilled_pages=[" + HashDistinct::join(grouping->sizes) + "]"; grouping.reset(); }
};

// a join condition: the whole condition for the nested loop, and for the equality joins the columns of
// each side that must be equal
struct JoinCondition { std::unique_ptr<Node> cond; std::vector<size_t> left, right; };

// block nested loop: fills a block of the outer, then streams the whole inner past it. The block takes
// frames from the pool as it grows, up to blockPages of them
struct BlockLoop {
    TupleSource& outer;
    TupleSource& inner;
    const std::vector<Col>& outerSchema;
    const Node* cond;
    unsigned long long& comparisons;
    int blockPages;
    Frames& memory;
    std::vector<Row> block;
    Row pending, innerRow;
    bool havePending = false, outerDone = false, scanning = false;
    size_t at = 0;
    long long blocks = 0;
    BlockLoop(TupleSource& o, TupleSource& i, const std::vector<Col>& s, const Node* c, unsigned long long& cmp, int pages, Frames& f)
        : outer(o), inner(i), outerSchema(s), cond(c), comparisons(cmp), blockPages(pages), memory(f) {
        if (pages < 1) throw Error{"error: a block nested loop needs at least one frame for its block, none is free"};
    }
    bool fill() {
        block.clear();
        Packing pack;
        if (!havePending && !outerDone) { havePending = outer.next(pending); outerDone = !havePending; }
        while (havePending && pack.add(encodedSize(outerSchema, pending), blockPages)) {
            if (memory.count() < pack.pages) memory.take(1);
            block.push_back(std::move(pending));
            havePending = outer.next(pending);
            outerDone = !havePending;
        }
        return !block.empty();
    }
    bool next(Row& out) {
        for (;;) {
            if (scanning) {
                while (at < block.size()) {
                    const Row& o = block[at++];
                    if (cond) { comparisons++; if (!eval(*cond, o, innerRow)) continue; }
                    out = joined(o, innerRow);
                    return true;
                }
                if (inner.next(innerRow)) { at = 0; continue; }
                scanning = false;
            }
            if (!fill()) return false;
            blocks++;
            inner.rewind();
            scanning = true;
            at = block.size();
        }
    }
};

struct NestedLoopJoin : Operator {
    JoinCondition join;
    std::unique_ptr<SpillFile> spill;
    std::unique_ptr<TupleSource> outer, inner;
    std::unique_ptr<BlockLoop> loop;
    Frames block;
    NestedLoopJoin(Database& d) : Operator(d, "nested_loop_join"), block(d.pool) {}
    bool holdsMemory() const override { return true; }
    void start() override {
        outer = inputOf(*this, 0, spill);
        inner = inputOf(*this, 1, spill);
        int pages = available() - outer->framesToCome() - inner->framesToCome();
        if (pages < 1) throw Error{"error: nested_loop_join needs at least 3 frames, " + std::to_string(available()) + " are free"};
        loop = std::make_unique<BlockLoop>(*outer, *inner, children[0]->schema, join.cond.get(), counters.comparisons, pages, block);
    }
    bool produce(Row& row) override { return loop->next(row); }
    void finish() override {
        notes = "block_pages=" + std::to_string(loop ? loop->blockPages : 0) + " blocks=" + std::to_string(loop ? loop->blocks : 0);
        loop.reset(); outer.reset(); inner.reset(); block.releaseAll(); spill.reset();
    }
};

// grace hash join: partition both inputs to disk, then join partition by partition; a build partition
// larger than the frames left for it is joined by block nested loop instead
struct HashJoin : Operator {
    JoinCondition join;
    std::unique_ptr<SpillFile> spill, inputSpill;
    std::vector<Run> leftParts, rightParts;
    size_t part = 0;
    Frames tableFrames;
    std::vector<Row> table;
    std::vector<std::vector<int>> buckets;
    std::unique_ptr<RunReader> probe;
    std::unique_ptr<RunSource> fallbackOuter, fallbackInner;
    std::unique_ptr<BlockLoop> fallback;
    Row s;
    const std::vector<int>* candidates = nullptr;
    size_t candidate = 0;
    int overflowed = 0;
    HashJoin(Database& d) : Operator(d, "hash_join"), tableFrames(d.pool) {}
    bool holdsMemory() const override { return true; }

    std::vector<Run> partition(size_t side, const std::vector<size_t>& keys, int count) {
        std::unique_ptr<TupleSource> in = inputOf(*this, side, inputSpill);
        std::vector<std::unique_ptr<RunWriter>> writers;
        for (int p = 0; p < count; p++) writers.push_back(std::make_unique<RunWriter>(db, spill->file, children[side]->schema));
        Row row;
        while (in->next(row)) writers[hashKey(row, keys, 1) % size_t(count)]->add(row);
        in.reset();
        inputSpill.reset();
        std::vector<Run> runs;
        for (auto& w : writers) runs.push_back(w->finish());
        return runs;
    }
    void start() override {
        spill = std::make_unique<SpillFile>(db);
        int count = available() - 1;
        if (count < 1) throw Error{"error: hash_join needs at least 2 frames, " + std::to_string(available()) + " are free"};
        leftParts = partition(0, join.left, count);
        rightParts = partition(1, join.right, count);
        std::string sizes;
        for (int p = 0; p < count; p++) sizes += (p ? "," : "") + std::to_string(leftParts[size_t(p)].pages.size()) + "/" + std::to_string(rightParts[size_t(p)].pages.size());
        notes = "partitions=" + std::to_string(count) + " pages_left/right=[" + sizes + "]";
        part = 0;
        overflowed = 0;
        startPartition();
    }
    void endPartition() { probe.reset(); fallback.reset(); fallbackOuter.reset(); fallbackInner.reset(); table.clear(); buckets.clear(); tableFrames.releaseAll(); candidates = nullptr; }
    void startPartition() {
        for (; part < leftParts.size(); part++) {
            const Run& r = leftParts[part];
            if (r.tuples == 0 || rightParts[part].tuples == 0) continue;
            int room = available() - 1;
            if (int(r.pages.size()) > room) {
                overflowed++;
                fallbackOuter = std::make_unique<RunSource>(db, r, children[0]->schema);
                fallbackInner = std::make_unique<RunSource>(db, rightParts[part], children[1]->schema);
                fallback = std::make_unique<BlockLoop>(*fallbackOuter, *fallbackInner, children[0]->schema, join.cond.get(), counters.comparisons, available(), tableFrames);
                return;
            }
            tableFrames.take(int(r.pages.size()));
            {
                RunReader build(db, children[0]->schema);
                build.start(r);
                Row row;
                buckets.assign(std::max<size_t>(1, size_t(r.tuples)), {});
                while (build.next(row)) {
                    buckets[hashKey(row, join.left, 2) % buckets.size()].push_back(int(table.size()));
                    table.push_back(std::move(row));
                }
            }
            probe = std::make_unique<RunReader>(db, children[1]->schema);
            probe->start(rightParts[part]);
            return;
        }
    }
    bool produce(Row& row) override {
        while (part < leftParts.size()) {
            if (fallback) {
                if (fallback->next(row)) return true;
            } else {
                while (candidates && candidate < candidates->size()) {
                    const Row& r = table[size_t((*candidates)[candidate++])];
                    counters.comparisons++;
                    if (sameKey(r, join.left, s, join.right)) { row = joined(r, s); return true; }
                }
                if (probe->next(s)) { candidates = &buckets[hashKey(s, join.right, 2) % buckets.size()]; candidate = 0; continue; }
            }
            endPartition();
            part++;
            startPartition();
        }
        return false;
    }
    void finish() override {
        notes += " overflowed=" + std::to_string(overflowed);
        endPartition(); leftParts.clear(); rightParts.clear(); spill.reset(); inputSpill.reset();
    }
};

// sorts each input to a single run on disk, then walks the two runs together, going back to the start of
// a group of equal keys on the right for every tuple of the matching group on the left. The merge holds
// three frames: one page of the left run, and the current and previous page of the right run
struct SortMergeJoin : Operator {
    JoinCondition join;
    std::unique_ptr<SpillFile> spill;
    std::unique_ptr<ExternalSort> leftSort, rightSort;
    Run leftRun, rightRun;
    std::unique_ptr<RunReader> l, r;
    Row a, b, groupFirst;
    bool haveA = false, haveB = false, inGroup = false;
    RunReader::Position mark{0, 0};
    SortMergeJoin(Database& d) : Operator(d, "sort_merge_join") {}
    bool holdsMemory() const override { return true; }
    int cmp(const Row& x, const Row& y) {
        counters.comparisons++;
        for (size_t i = 0; i < join.left.size(); i++) if (int d = compare(x[join.left[i]], y[join.right[i]])) return d;
        return 0;
    }
    int cmpLeft(const Row& x, const Row& y) {
        counters.comparisons++;
        for (size_t k : join.left) if (int d = compare(x[k], y[k])) return d;
        return 0;
    }
    Run sortSide(size_t side, const std::vector<size_t>& keys, std::unique_ptr<ExternalSort>& sorter) {
        std::unique_ptr<TupleSource> in = inputOf(*this, side, spill);
        std::vector<SortKey> sortKeys;
        for (size_t k : keys) sortKeys.push_back({k, false});
        sorter = std::make_unique<ExternalSort>(db, children[side]->schema, sortKeys, counters.comparisons, parentFrames);
        sorter->generate(*in);
        in.reset();
        return sorter->single();
    }
    void start() override {
        leftRun = sortSide(0, join.left, leftSort);
        rightRun = sortSide(1, join.right, rightSort);
        notes = "passes=" + std::to_string(leftSort->passes) + "/" + std::to_string(rightSort->passes);
        l = std::make_unique<RunReader>(db, children[0]->schema);
        r = std::make_unique<RunReader>(db, children[1]->schema, true);
        l->start(leftRun);
        r->start(rightRun);
        haveA = l->next(a);
        haveB = r->next(b);
        inGroup = false;
    }
    bool produce(Row& row) override {
        for (;;) {
            if (inGroup) {
                if (haveB && cmp(a, b) == 0) { row = joined(a, b); haveB = r->next(b); return true; }
                haveA = l->next(a);
                if (haveA && cmpLeft(a, groupFirst) == 0) { r->seek(mark); haveB = r->next(b); continue; }
                inGroup = false;
            }
            if (!haveA || !haveB) return false;
            int d = cmp(a, b);
            if (d < 0) haveA = l->next(a);
            else if (d > 0) haveB = r->next(b);
            else { inGroup = true; groupFirst = a; mark = r->last(); }
        }
    }
    void finish() override { l.reset(); r.reset(); leftSort.reset(); rightSort.reset(); leftRun = {}; rightRun = {}; spill.reset(); }
};

// union removes duplicates by sorting the two inputs together
struct SortUnion : Operator {
    std::unique_ptr<SpillFile> spill;
    std::unique_ptr<ExternalSort> sorter;
    Row last;
    bool any = false;
    SortUnion(Database& d) : Operator(d, "union") {}
    bool holdsMemory() const override { return true; }
    // the left input then the right, the right only opened once the left is used up
    struct Both : TupleSource {
        SortUnion& op;
        std::unique_ptr<Run> right;
        std::unique_ptr<TupleSource> current;
        size_t side = 0;
        Both(SortUnion& u, std::unique_ptr<Run> r) : op(u), right(std::move(r)), current(inputOf(u, 0, u.spill)) {}
        bool next(Row& row) override {
            if (current->next(row)) return true;
            if (side == 1) return false;
            current.reset();
            side = 1;
            if (right) current = std::make_unique<RunSource>(op.db, std::move(*right), op.children[1]->schema);
            else current = std::make_unique<ChildSource>(*op.children[1]);
            return current->next(row);
        }
        void rewind() override {}
        void close() override { current.reset(); }
        int framesToCome() const override { return 1; }
    };
    void start() override {
        std::unique_ptr<Run> right;
        if (materialize[1]) right = std::make_unique<Run>(materialized(*this, 1, spill));
        sorter = std::make_unique<ExternalSort>(db, schema, allColumns(schema.size()), counters.comparisons, parentFrames);
        {
            Both in(*this, std::move(right));
            sorter->generate(in);
        }
        spill.reset();
        sorter->merge(false);
        sorter->startFinal();
        any = false;
    }
    bool produce(Row& row) override {
        while (sorter->next(row)) {
            if (any) { counters.comparisons++; if (same(row, last)) continue; }
            last = row;
            any = true;
            return true;
        }
        return false;
    }
    void finish() override { sorter.reset(); }
};

// intersect and except: each input sorted to one run, then walked together, every tuple at most once
struct SetDifference : Operator {
    bool intersect;
    std::unique_ptr<SpillFile> spill;
    std::unique_ptr<ExternalSort> leftSort, rightSort;
    Run leftRun, rightRun;
    std::unique_ptr<RunReader> l, r;
    Row a, b;
    bool haveA = false, haveB = false;
    SetDifference(Database& d, bool isIntersect) : Operator(d, isIntersect ? "intersect" : "except"), intersect(isIntersect) {}
    bool holdsMemory() const override { return true; }
    Run sortSide(size_t side, std::unique_ptr<ExternalSort>& sorter) {
        std::unique_ptr<TupleSource> in = inputOf(*this, side, spill);
        sorter = std::make_unique<ExternalSort>(db, children[side]->schema, allColumns(schema.size()), counters.comparisons, parentFrames);
        sorter->generate(*in);
        in.reset();
        return sorter->single();
    }
    void start() override {
        leftRun = sortSide(0, leftSort);
        rightRun = sortSide(1, rightSort);
        l = std::make_unique<RunReader>(db, children[0]->schema);
        r = std::make_unique<RunReader>(db, children[1]->schema);
        l->start(leftRun);
        r->start(rightRun);
        haveA = l->next(a);
        haveB = r->next(b);
    }
    bool produce(Row& row) override {
        while (haveA) {
            bool onRight = false;
            while (haveB) {
                counters.comparisons++;
                int d = compareRow(a, b);
                if (d > 0) { haveB = r->next(b); continue; }
                onRight = d == 0;
                break;
            }
            row = a;
            for (;;) {
                haveA = l->next(a);
                if (!haveA) break;
                counters.comparisons++;
                if (!same(a, row)) break;
            }
            if (onRight == intersect) return true;
        }
        return false;
    }
    void finish() override { l.reset(); r.reset(); leftSort.reset(); rightSort.reset(); leftRun = {}; rightRun = {}; spill.reset(); }
};
