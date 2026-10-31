// the only code in the system that touches a file: 4096 byte pages addressed by file and page number
#pragma once
#include "lexer.hpp"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

constexpr int PAGE_SIZE = 4096;

struct PageId {
    uint32_t file, page;
    bool operator==(const PageId& o) const { return file == o.file && page == o.page; }
    bool operator<(const PageId& o) const { return file != o.file ? file < o.file : page < o.page; }
};
struct IoCounters { unsigned long long reads = 0, writes = 0; };

struct DiskManager {
    struct File { std::string path; int fd; uint32_t pages; };
    std::vector<File> files;
    IoCounters total;
    // the executor points this at the operator that is running, so its own temporary files are charged to it
    IoCounters* attributed = nullptr;

    DiskManager() = default;
    DiskManager(const DiskManager&) = delete;
    ~DiskManager() { for (File& f : files) if (f.fd >= 0) ::close(f.fd); }

    [[noreturn]] static void fail(const std::string& what, const std::string& path) { throw Error{"io error: " + what + " " + path + ": " + std::strerror(errno)}; }
    uint32_t open(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0) fail("cannot open", path);
        off_t size = ::lseek(fd, 0, SEEK_END);
        files.push_back({path, fd, uint32_t(size / PAGE_SIZE)});
        return uint32_t(files.size() - 1);
    }
    uint32_t createTemp(const std::string& dir) {
        std::string path = dir + "/.tmp-XXXXXX";
        int fd = ::mkstemp(path.data());
        if (fd < 0) fail("cannot create a temporary file in", dir);
        files.push_back({path, fd, 0});
        return uint32_t(files.size() - 1);
    }
    void remove(uint32_t file) {
        ::close(files[file].fd);
        ::unlink(files[file].path.c_str());
        files[file].fd = -1;
    }
    void truncate(uint32_t file, uint32_t pages) {
        if (::ftruncate(files[file].fd, off_t(pages) * PAGE_SIZE) != 0) fail("cannot resize", files[file].path);
        files[file].pages = pages;
    }
    void count(bool write) {
        (write ? total.writes : total.reads)++;
        if (attributed) (write ? attributed->writes : attributed->reads)++;
    }
    void readPage(PageId id, uint8_t* out) {
        ssize_t n = ::pread(files[id.file].fd, out, PAGE_SIZE, off_t(id.page) * PAGE_SIZE);
        if (n < 0) fail("cannot read", files[id.file].path);
        std::memset(out + n, 0, PAGE_SIZE - size_t(n));
        count(false);
    }
    void writePage(PageId id, const uint8_t* data) {
        if (::pwrite(files[id.file].fd, data, PAGE_SIZE, off_t(id.page) * PAGE_SIZE) != PAGE_SIZE) fail("cannot write", files[id.file].path);
        count(true);
    }
    uint32_t allocatePage(uint32_t file) { return files[file].pages++; }
    uint32_t numPages(uint32_t file) const { return files[file].pages; }
};
