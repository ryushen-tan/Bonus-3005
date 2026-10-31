// heap files: a chain of directory pages listing data pages and their free space, page 0 first
#pragma once
#include "buffer.hpp"
#include "page.hpp"

// directory pages are allocated in order, so a link that does not lead to a later page means the file is corrupt
inline void checkLink(const DiskManager& disk, uint32_t file, uint32_t dirNo, uint32_t next) {
    if (next != NO_PAGE && next <= dirNo)
        throw Error{"error: corrupt table file " + disk.files[file].path + ": directory page " + std::to_string(dirNo) + " links back to page " + std::to_string(next)};
}

struct HeapFile {
    BufferPool& pool;
    uint32_t file;
    uint32_t& dataPages;
    uint32_t& liveTuples;
    std::string tableName;

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
    // a second page while directory page dirNo is pinned; when there is no frame for it, dirNo is let go first
    template <class Take> auto besideDirectory(uint32_t dirNo, Take take) {
        try { return take(); } catch (const Error&) { pool.unpin({file, dirNo}, false); throw; }
    }
    // first fit over the directory, a new data page when nothing has room
    Rid insert(std::string_view record) {
        if (record.size() > size_t(MAX_RECORD))
            throw Error{"error: table " + tableName + ": a record of " + std::to_string(record.size()) + " bytes does not fit on a page (at most " + std::to_string(MAX_RECORD) + ")"};
        int need = int(record.size()) + SLOT_SIZE;
        for (uint32_t dirNo = 0;;) {
            uint8_t* dir = pool.fetch({file, dirNo});
            for (int i = 0; i < entryCount(dir); i++) {
                if (entryFree(dir, i) < need) continue;
                uint32_t pageNo = entryPage(dir, i);
                uint8_t* data = besideDirectory(dirNo, [&] { return pool.fetch({file, pageNo}); });
                int slot = insertRecord(data, record);
                setEntryFree(dir, i, freeSpace(data));
                pool.unpin({file, pageNo}, true);
                pool.unpin({file, dirNo}, true);
                liveTuples++;
                return {pageNo, uint32_t(slot)};
            }
            uint32_t next = nextDirPage(dir);
            if (next != NO_PAGE) { pool.unpin({file, dirNo}, false); checkLink(pool.disk, file, dirNo, next); dirNo = next; continue; }
            if (entryCount(dir) == DIR_CAPACITY) {
                uint32_t newDir;
                initDirPage(besideDirectory(dirNo, [&] { return pool.newPage(file, newDir); }), newDir);
                put32(dir + 4, newDir);
                pool.unpin({file, dirNo}, true);
                pool.unpin({file, newDir}, true);
                dirNo = newDir;
                continue;
            }
            Rid rid = besideDirectory(dirNo, [&] { return insertIntoNewPage(dir, record); });
            pool.unpin({file, dirNo}, true);
            liveTuples++;
            return rid;
        }
    }
    // a rid's page has to be one the directory lists, or a directory page could be read and written as data
    void checkDataPage(Rid rid) {
        for (uint32_t dirNo = 0; dirNo != NO_PAGE;) {
            const uint8_t* dir = pool.fetch({file, dirNo});
            bool listed = false;
            for (int i = 0; i < entryCount(dir) && !listed; i++) listed = entryPage(dir, i) == rid.page;
            uint32_t next = nextDirPage(dir);
            pool.unpin({file, dirNo}, false);
            if (listed) return;
            checkLink(pool.disk, file, dirNo, next);
            dirNo = next;
        }
        throw Error{"error: no record at rid " + ridText(rid) + ", page " + std::to_string(rid.page) + " is not a data page of table " + tableName};
    }
    std::string get(Rid rid) {
        checkDataPage(rid);
        uint8_t* data = pool.fetch({file, rid.page});
        std::string record;
        try { record = std::string(recordAt(data, rid.slot)); } catch (...) { pool.unpin({file, rid.page}, false); throw; }
        pool.unpin({file, rid.page}, false);
        return record;
    }
    // the slot dies and its bytes stay where they are, so the page's free space and its directory entry don't change
    void remove(Rid rid) {
        checkDataPage(rid);
        uint8_t* data = pool.fetch({file, rid.page});
        try { deleteRecord(data, rid.slot); } catch (...) { pool.unpin({file, rid.page}, false); throw; }
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
            checkLink(pool.disk, file, dirNo, nextDir);
        }
    }
    void close() { if (page) { pool.unpin({file, pageNo}, false); page = nullptr; } }
};
