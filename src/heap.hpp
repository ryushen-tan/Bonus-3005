// heap files: a chain of directory pages listing data pages and their free space, page 0 first
#pragma once
#include "buffer.hpp"
#include "page.hpp"

struct HeapFile {
    BufferPool& pool;
    uint32_t file;
    uint32_t& dataPages;
    uint32_t& liveTuples;

    static void create(BufferPool& pool, uint32_t file) {
        uint32_t pageNo;
        initDirPage(pool.newPage(file, pageNo), pageNo);
        pool.unpin({file, pageNo}, true);
    }
    // a new data page entered in the directory page dir, which is pinned
    Rid insertIntoNewPage(uint8_t* dir, std::string_view record) {
        uint32_t pageNo;
        uint8_t* data = pool.newPage(file, pageNo);
        initDataPage(data, pageNo);
        int slot = insertRecord(data, record);
        addEntry(dir, pageNo, freeSpace(data));
        pool.unpin({file, pageNo}, true);
        dataPages++;
        return {pageNo, uint32_t(slot)};
    }
    // first fit over the directory, a new data page when nothing has room
    Rid insert(std::string_view record) {
        int need = int(record.size()) + SLOT_SIZE;
        for (uint32_t dirNo = 0;;) {
            uint8_t* dir = pool.fetch({file, dirNo});
            for (int i = 0; i < entryCount(dir); i++) {
                if (entryFree(dir, i) < need) continue;
                uint32_t pageNo = entryPage(dir, i);
                uint8_t* data = pool.fetch({file, pageNo});
                int slot = insertRecord(data, record);
                setEntryFree(dir, i, freeSpace(data));
                pool.unpin({file, pageNo}, true);
                pool.unpin({file, dirNo}, true);
                liveTuples++;
                return {pageNo, uint32_t(slot)};
            }
            uint32_t next = nextDirPage(dir);
            if (next != NO_PAGE) { pool.unpin({file, dirNo}, false); dirNo = next; continue; }
            if (entryCount(dir) == DIR_CAPACITY) {
                uint32_t newDir;
                initDirPage(pool.newPage(file, newDir), newDir);
                put32(dir + 4, newDir);
                pool.unpin({file, dirNo}, true);
                pool.unpin({file, newDir}, true);
                dirNo = newDir;
                continue;
            }
            Rid rid = insertIntoNewPage(dir, record);
            pool.unpin({file, dirNo}, true);
            liveTuples++;
            return rid;
        }
    }
    std::string get(Rid rid) {
        uint8_t* data = pool.fetch({file, rid.page});
        std::string record;
        try { record = std::string(recordAt(data, int(rid.slot))); } catch (...) { pool.unpin({file, rid.page}, false); throw; }
        pool.unpin({file, rid.page}, false);
        return record;
    }
    // the slot dies and its bytes stay where they are, so the page's free space and its directory entry don't change
    void remove(Rid rid) {
        uint8_t* data = pool.fetch({file, rid.page});
        try { deleteRecord(data, int(rid.slot)); } catch (...) { pool.unpin({file, rid.page}, false); throw; }
        pool.unpin({file, rid.page}, true);
        liveTuples--;
    }
};

// heap order, one page pinned at a time: a directory page's list of data pages is copied out and the
// directory page unpinned before its first data page is fetched
struct HeapScan {
    BufferPool& pool;
    uint32_t file;
    uint32_t nextDir = 0;
    std::vector<uint32_t> pages;
    size_t nextEntry = 0;
    uint8_t* page = nullptr;
    uint32_t pageNo = 0;
    int slot = 0;

    HeapScan(BufferPool& p, uint32_t f) : pool(p), file(f) {}
    HeapScan(const HeapScan&) = delete;
    ~HeapScan() { close(); }

    bool next(Rid& rid, std::string_view& record) {
        for (;;) {
            if (page) {
                while (slot < slotCount(page)) {
                    int s = slot++;
                    if (!live(page, s)) continue;
                    rid = {pageNo, uint32_t(s)};
                    record = {(const char*)page + slotOffset(page, s), size_t(slotLength(page, s))};
                    return true;
                }
                close();
            }
            if (nextEntry < pages.size()) { pageNo = pages[nextEntry++]; page = pool.fetch({file, pageNo}); slot = 0; continue; }
            if (nextDir == NO_PAGE) return false;
            const uint8_t* dir = pool.fetch({file, nextDir});
            pages.clear();
            for (int i = 0; i < entryCount(dir); i++) pages.push_back(entryPage(dir, i));
            nextEntry = 0;
            uint32_t dirNo = nextDir;
            nextDir = nextDirPage(dir);
            pool.unpin({file, dirNo}, false);
        }
    }
    void close() { if (page) { pool.unpin({file, pageNo}, false); page = nullptr; } }
};
