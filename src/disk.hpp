// the only code in the system that touches a file: 4096 byte pages addressed by file and page number
#pragma once
#include "lexer.hpp"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

constexpr int PAGE_SIZE = 4096;

struct PageId {
    uint32_t file, page;
    bool operator==(const PageId& o) const { return file == o.file && page == o.page; }
    bool operator<(const PageId& o) const { return file != o.file ? file < o.file : page < o.page; }
};
struct IoCounters { unsigned long long reads = 0, writes = 0; };

struct DiskManager {
    // unopened: why the file could not be opened, given as the error of every use of it
    struct File { std::string path; int fd; uint32_t pages; std::string unopened; };
    std::vector<File> files;
    IoCounters total;
    // the executor points this at the operator that is running, so its own temporary files are charged to it
    IoCounters* attributed = nullptr;

    DiskManager() = default;
    DiskManager(const DiskManager&) = delete;
    ~DiskManager() { for (File& f : files) if (f.fd >= 0) ::close(f.fd); }

    [[noreturn]] static void fail(const std::string& what, const std::string& path) { throw Error{"io error: " + what + " " + path + ": " + std::strerror(errno)}; }
    // only a file that exists, so a missing table file is an error rather than a new empty table. The error is
    // given when the file is first used, so the other tables of a database with a missing file still work
    uint32_t open(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDWR);
        if (fd < 0) files.push_back({path, fd, 0, "io error: cannot open " + path + ": " + std::strerror(errno)});
        else files.push_back({path, fd, uint32_t(::lseek(fd, 0, SEEK_END) / PAGE_SIZE), ""});
        return uint32_t(files.size() - 1);
    }
    // a file that must not exist yet, so that creating one never overwrites another
    uint32_t create(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
        if (fd < 0) fail("cannot create", path);
        files.push_back({path, fd, 0, ""});
        return uint32_t(files.size() - 1);
    }
    uint32_t createTemp(const std::string& dir) {
        std::string path = dir + "/.tmp-XXXXXX";
        int fd = ::mkstemp(path.data());
        if (fd < 0) fail("cannot create a temporary file in", dir);
        files.push_back({path, fd, 0, ""});
        return uint32_t(files.size() - 1);
    }
    void close(uint32_t file) {
        ::close(files[file].fd);
        files[file].fd = -1;
    }
    void remove(uint32_t file) {
        ::unlink(files[file].path.c_str());
        close(file);
    }
    // rename(2) replaces the file at path in one step, so a reader finds the old file or the new one. The file
    // renamed is a temporary one, which mkstemp made 0600, so it first gets the mode create gives every other
    // file: 0644 less the umask, which umask(2) can only read by setting it, so it is set back at once
    void rename(uint32_t file, const std::string& path) {
        mode_t mask = ::umask(0);
        ::umask(mask);
        if (::fchmod(files[file].fd, 0644 & ~mask) != 0) fail("cannot set the mode of", files[file].path);
        if (::rename(files[file].path.c_str(), path.c_str()) != 0) fail("cannot rename " + files[file].path + " to", path);
        files[file].path = path;
    }
    void truncate(uint32_t file, uint32_t pages) {
        if (::ftruncate(files[file].fd, off_t(pages) * PAGE_SIZE) != 0) fail("cannot resize", files[file].path);
        files[file].pages = pages;
    }
    void count(bool write) {
        (write ? total.writes : total.reads)++;
        if (attributed) (write ? attributed->writes : attributed->reads)++;
    }
    File& usable(uint32_t file) {
        if (!files[file].unopened.empty()) throw Error{files[file].unopened};
        return files[file];
    }
    void readPage(PageId id, uint8_t* out) {
        const File& file = usable(id.file);
        if (id.page >= file.pages) throw Error{"io error: " + file.path + " has " + std::to_string(file.pages) + " pages, there is no page " + std::to_string(id.page)};
        ssize_t n = ::pread(file.fd, out, PAGE_SIZE, off_t(id.page) * PAGE_SIZE);
        if (n < 0) fail("cannot read", file.path);
        if (n != PAGE_SIZE) throw Error{"io error: " + file.path + " ends inside page " + std::to_string(id.page)};
        count(false);
    }
    void writePage(PageId id, const uint8_t* data) {
        const File& file = usable(id.file);
        if (::pwrite(file.fd, data, PAGE_SIZE, off_t(id.page) * PAGE_SIZE) != PAGE_SIZE) fail("cannot write", file.path);
        count(true);
    }
    uint32_t allocatePage(uint32_t file) { return usable(file).pages++; }
    uint32_t numPages(uint32_t file) { return usable(file).pages; }
};

inline void createDirectories(const std::string& dir) {
    std::error_code failure;
    std::filesystem::create_directories(dir, failure);
    if (failure) throw Error{"io error: cannot create directory " + dir + ": " + failure.message()};
}

// a scratch file that is deleted however the code holding it exits
struct TempFile {
    DiskManager& disk;
    uint32_t file;
    TempFile(DiskManager& d, const std::string& dir) : disk(d), file(d.createTemp(dir)) {}
    TempFile(const TempFile&) = delete;
    ~TempFile() { disk.remove(file); }
};
