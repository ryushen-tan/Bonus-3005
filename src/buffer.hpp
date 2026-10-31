// B frames over the disk manager: pinning, dirty tracking, LRU or CLOCK, and frames reserved as working memory
#pragma once
#include "disk.hpp"
#include <array>
#include <map>

enum Policy { Lru, Clock };
struct PoolStats { unsigned long long hits, misses, evictions, reads, writes; };

struct BufferPool {
    // a reserved frame holds no page; it is an operator's working memory and counts as pinned
    struct Frame { PageId id{0, 0}; bool used = false, reserved = false, dirty = false, ref = false; int pins = 0; unsigned long long lastUse = 0; std::array<uint8_t, PAGE_SIZE> data{}; };
    DiskManager& disk;
    Policy policy;
    std::vector<Frame> frames;
    std::map<PageId, size_t> table;
    size_t hand = 0;
    unsigned long long accesses = 0, hits = 0, misses = 0, evictions = 0;
    int held = 0, peak = 0;

    // every tool's --frames B ends up here, so this is the one check of it
    BufferPool(DiskManager& d, int frameCount, Policy p) : disk(d), policy(p), frames(size_t(frameCount)) {
        if (frameCount < 1) throw Error{"error: --frames must be at least 1"};
    }

    static constexpr size_t NONE = size_t(-1);
    size_t chooseVictim() {
        for (size_t i = 0; i < frames.size(); i++) if (!frames[i].used && !frames[i].reserved) return i;
        if (policy == Lru) {
            size_t victim = NONE;
            for (size_t i = 0; i < frames.size(); i++)
                if (frames[i].pins == 0 && (victim == NONE || frames[i].lastUse < frames[victim].lastUse)) victim = i;
            return victim;
        }
        for (size_t swept = 0;;) {
            Frame& f = frames[hand];
            size_t at = hand;
            hand = (hand + 1) % frames.size();
            if (f.pins > 0) swept++;
            else if (f.ref) { f.ref = false; swept++; }
            else return at;
            if (swept > 2 * frames.size()) return NONE;
        }
    }
    size_t takeFrame() {
        size_t v = chooseVictim();
        if (v == NONE) throw Error{"error: no unpinned frame available, all " + std::to_string(frames.size()) + " frames are pinned"};
        Frame& f = frames[v];
        if (f.used) {
            if (f.dirty) disk.writePage(f.id, f.data.data());
            table.erase(f.id);
        }
        return v;
    }
    void hold() { peak = std::max(peak, ++held); }
    void loaded(size_t v, PageId id, bool dirty) {
        Frame& f = frames[v];
        f.id = id; f.used = true; f.dirty = dirty; f.pins = 1; f.ref = true; f.lastUse = ++accesses;
        table[id] = v;
        hold();
        if (policy == Clock) hand = (v + 1) % frames.size();
    }

    uint8_t* fetch(PageId id) {
        auto it = table.find(id);
        if (it != table.end()) {
            Frame& f = frames[it->second];
            if (f.pins++ == 0) hold();
            f.ref = true;
            f.lastUse = ++accesses;
            hits++;
            return f.data.data();
        }
        size_t v = takeFrame();
        if (frames[v].used) evictions++;
        disk.readPage(id, frames[v].data.data());
        misses++;
        loaded(v, id, false);
        return frames[v].data.data();
    }
    void unpin(PageId id, bool dirty) {
        auto it = table.find(id);
        std::string page = "(" + std::to_string(id.file) + ", " + std::to_string(id.page) + ")";
        if (it == table.end()) throw Error{"error: unpin of page " + page + " which is not in the pool"};
        Frame& f = frames[it->second];
        if (f.pins == 0) throw Error{"error: unpin of page " + page + " whose pin count is already 0"};
        f.dirty = f.dirty || dirty;
        if (--f.pins == 0) held--;
    }
    // zero filled, pinned and dirty
    uint8_t* newPage(uint32_t file, uint32_t& pageNo) {
        size_t v = takeFrame();
        pageNo = disk.allocatePage(file);
        frames[v].data.fill(0);
        loaded(v, {file, pageNo}, true);
        return frames[v].data.data();
    }
    void flushAll() {
        for (Frame& f : frames) if (f.used && f.dirty) { disk.writePage(f.id, f.data.data()); f.dirty = false; }
    }

    size_t reserve() {
        size_t v = takeFrame();
        Frame& f = frames[v];
        f.used = false; f.reserved = true; f.dirty = false; f.pins = 1;
        hold();
        return v;
    }
    void release(size_t v) { Frame& f = frames[v]; f.reserved = false; f.pins = 0; held--; }
    uint8_t* memory(size_t v) { return frames[v].data.data(); }
    int freeFrames() const { return int(frames.size()) - held; }

    // forgets a file's pages without writing them, before the file is rewritten or deleted
    void dropFile(uint32_t file) {
        for (Frame& f : frames) if (f.used && f.id.file == file) { table.erase(f.id); f.used = false; f.dirty = false; }
    }
    // writes everything back and empties the pool, so a command's counters start from a known state
    void clear() {
        flushAll();
        for (Frame& f : frames) if (f.used && f.pins == 0) { table.erase(f.id); f.used = false; }
        hand = 0;
        hits = misses = evictions = 0;
        disk.total = {};
        peak = held;
    }
    PoolStats stats() const { return {hits, misses, evictions, disk.total.reads, disk.total.writes}; }
};
