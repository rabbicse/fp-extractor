// SPDX-License-Identifier: MIT
// Durable template store: an append-only log of (put | delete) records with a
// CRC32 per record. Loading 1 M templates is a single sequential read (a few
// seconds), writes are O(1) appends, and a torn last record (crash during
// write) is detected and ignored. `compact()` rewrites only live records.
//
// This is deliberately simple and dependency-free. For multi-node deployments
// put PostgreSQL (or any DB) in front as the system of record and let each
// matcher node rebuild its in-memory Gallery from it - see docs/SCALING.md.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fpx {

class TemplateStore {
public:
    using PutFn = std::function<void(const std::string& id, const std::vector<uint8_t>& record)>;
    using EraseFn = std::function<void(const std::string& id)>;

    /// Opens (creating if needed) the store file.
    explicit TemplateStore(std::string path);
    ~TemplateStore();
    TemplateStore(const TemplateStore&) = delete;
    TemplateStore& operator=(const TemplateStore&) = delete;

    /// Replay the log in order. Returns number of records replayed.
    /// A truncated/corrupt tail is reported via `warning` and ignored.
    size_t replay(const PutFn& on_put, const EraseFn& on_erase, std::string* warning = nullptr);

    void put(const std::string& id, const std::vector<uint8_t>& record);
    void erase(const std::string& id);
    /// Flush to the OS and fsync.
    void sync();
    /// Rewrite the file with live records only (atomic rename).
    void compact();

    const std::string& path() const { return path_; }

private:
    void open_append();
    void append(uint8_t op, const std::string& id, const std::vector<uint8_t>& data);
    std::string path_;
    std::FILE* f_ = nullptr;
    std::mutex mu_;
};

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0);

/// Store record key for (subject, finger). Finger 0 keeps the bare subject id,
/// so single-finger stores written by earlier versions load unchanged.
/// Other fingers: "<subject>\x1F<position>" (unit separator, never valid in ids).
std::string make_store_key(const std::string& subject, uint8_t finger);
/// Inverse of make_store_key. Throws fpx::Error on a malformed position.
std::pair<std::string, uint8_t> split_store_key(const std::string& key);
/// Subject ids: 1..256 bytes, no control characters (and so no '\x1F').
bool valid_subject_id(const std::string& id);

}  // namespace fpx
