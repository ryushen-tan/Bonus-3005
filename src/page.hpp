// byte layouts: records, slotted data pages and heap file directory pages, all little-endian
#pragma once
#include "algebra.hpp"
#include "disk.hpp"
#include <cstdio>
#include <string_view>

inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); }

// a record is its fields in schema order: INT as 4 bytes, VARCHAR as a 2 byte length then the bytes,
// and REAL (only ever an AVG in a temporary file) as the 8 bytes of a double
inline Value intValue(long long i) { return {true, double(i), std::to_string(i)}; }
inline Value realValue(double d) { char text[64]; std::snprintf(text, sizeof text, "%.4f", d); return {true, d, text}; }
inline void encodeInto(std::string& out, const std::vector<Col>& cols, const Row& row) {
    for (size_t i = 0; i < cols.size(); i++) {
        uint8_t bytes[8];
        if (cols[i].type == StrT) {
            put16(bytes, uint16_t(row[i].text.size()));
            out.append((const char*)bytes, 2);
            out += row[i].text;
        } else if (cols[i].type == RealT) {
            std::memcpy(bytes, &row[i].num, 8);
            out.append((const char*)bytes, 8);
        } else {
            put32(bytes, uint32_t(int32_t(row[i].num)));
            out.append((const char*)bytes, 4);
        }
    }
}
inline std::string encode(const std::vector<Col>& cols, const Row& row) { std::string out; encodeInto(out, cols, row); return out; }
inline Row decode(const std::vector<Col>& cols, std::string_view record) {
    Row row;
    row.reserve(cols.size());
    const uint8_t* p = (const uint8_t*)record.data();
    for (const Col& c : cols) {
        if (c.type == StrT) { uint16_t n = get16(p); row.push_back({false, 0, std::string((const char*)p + 2, n)}); p += 2 + n; }
        else if (c.type == RealT) { double d; std::memcpy(&d, p, 8); row.push_back(realValue(d)); p += 8; }
        else { row.push_back(intValue(int32_t(get32(p)))); p += 4; }
    }
    return row;
}
inline size_t encodedSize(const std::vector<Col>& cols, const Row& row) {
    size_t n = 0;
    for (size_t i = 0; i < cols.size(); i++) n += cols[i].type == StrT ? 2 + row[i].text.size() : cols[i].type == RealT ? 8 : 4;
    return n;
}

// data page: page_id u32, slot_count u16, free_end u16, then 4 byte slots (offset u16, length u16)
constexpr int DATA_HEADER = 8, SLOT_SIZE = 4, MAX_RECORD = PAGE_SIZE - DATA_HEADER, NO_SPACE = -1;
struct Rid { uint32_t page, slot; };
inline std::string ridText(Rid r) { return "(" + std::to_string(r.page) + ", " + std::to_string(r.slot) + ")"; }

inline uint32_t pageIdOf(const uint8_t* page) { return get32(page); }
inline int slotCount(const uint8_t* page) { return get16(page + 4); }
inline int freeEnd(const uint8_t* page) { return get16(page + 6); }
inline int slotOffset(const uint8_t* page, int slot) { return get16(page + DATA_HEADER + SLOT_SIZE * slot); }
inline int slotLength(const uint8_t* page, int slot) { return get16(page + DATA_HEADER + SLOT_SIZE * slot + 2); }
inline int freeSpace(const uint8_t* page) { return freeEnd(page) - (DATA_HEADER + SLOT_SIZE * slotCount(page)); }
inline void initDataPage(uint8_t* page, uint32_t id) {
    std::memset(page, 0, PAGE_SIZE);
    put32(page, id);
    put16(page + 4, 0);
    put16(page + 6, PAGE_SIZE);
}
inline int insertRecord(uint8_t* page, std::string_view record) {
    int length = int(record.size()), slot = slotCount(page);
    if (freeSpace(page) < length + SLOT_SIZE) return NO_SPACE;
    int end = freeEnd(page) - length;
    std::memcpy(page + end, record.data(), record.size());
    put16(page + 6, uint16_t(end));
    put16(page + DATA_HEADER + SLOT_SIZE * slot, uint16_t(end));
    put16(page + DATA_HEADER + SLOT_SIZE * slot + 2, uint16_t(length));
    put16(page + 4, uint16_t(slot + 1));
    return slot;
}
inline bool live(const uint8_t* page, int slot) { return slotOffset(page, slot) != 0; }
inline std::string_view recordAt(const uint8_t* page, int slot) {
    Rid rid{pageIdOf(page), uint32_t(slot)};
    if (slot >= slotCount(page)) throw Error{"error: no record at rid " + ridText(rid) + ", the page has " + std::to_string(slotCount(page)) + " slots"};
    if (!live(page, slot)) throw Error{"error: rid " + ridText(rid) + " is a deleted record"};
    return {(const char*)page + slotOffset(page, slot), size_t(slotLength(page, slot))};
}
inline void deleteRecord(uint8_t* page, int slot) {
    recordAt(page, slot);
    put32(page + DATA_HEADER + SLOT_SIZE * slot, 0);
}

// directory page: page_id u32, next_dir_page u32, entry_count u16, then 6 byte entries (data_page_id u32, free_bytes u16)
constexpr uint32_t NO_PAGE = 0xFFFFFFFF;
constexpr int DIR_HEADER = 10, DIR_ENTRY = 6, DIR_CAPACITY = (PAGE_SIZE - DIR_HEADER) / DIR_ENTRY;
inline uint32_t nextDirPage(const uint8_t* dir) { return get32(dir + 4); }
inline int entryCount(const uint8_t* dir) { return get16(dir + 8); }
inline uint32_t entryPage(const uint8_t* dir, int i) { return get32(dir + DIR_HEADER + DIR_ENTRY * i); }
inline int entryFree(const uint8_t* dir, int i) { return get16(dir + DIR_HEADER + DIR_ENTRY * i + 4); }
inline void setEntryFree(uint8_t* dir, int i, int free) { put16(dir + DIR_HEADER + DIR_ENTRY * i + 4, uint16_t(free)); }
inline void initDirPage(uint8_t* dir, uint32_t id) {
    std::memset(dir, 0, PAGE_SIZE);
    put32(dir, id);
    put32(dir + 4, NO_PAGE);
}
inline void addEntry(uint8_t* dir, uint32_t dataPage, int free) {
    int i = entryCount(dir);
    put32(dir + DIR_HEADER + DIR_ENTRY * i, dataPage);
    setEntryFree(dir, i, free);
    put16(dir + 8, uint16_t(i + 1));
}
