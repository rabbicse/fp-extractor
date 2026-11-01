// SPDX-License-Identifier: MIT
#include "fpx/store.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>

#include "fpx/image.hpp"  // fpx::Error

#if defined(_WIN32)
#include <io.h>
#define fpx_fsync(fd) _commit(fd)
#define fpx_fileno _fileno
#else
#include <unistd.h>
#define fpx_fsync(fd) fsync(fd)
#define fpx_fileno fileno
#endif

namespace fpx {
namespace {

constexpr char kMagic[4] = {'F', 'P', 'X', 'S'};
constexpr uint32_t kVersion = 1;
constexpr uint8_t kOpPut = 1, kOpErase = 2;
// Record: op u8 | id_len u16 | data_len u32 | id | data | crc32 u32 (over op..data), little-endian.
constexpr size_t kRecHeader = 1 + 2 + 4;

void le16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}
void le32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

std::vector<uint8_t> encode_record(uint8_t op, const std::string& id, const std::vector<uint8_t>& data) {
    if (id.empty() || id.size() > 0xFFFF) throw Error("store: id must be 1..65535 bytes");
    std::vector<uint8_t> r;
    r.reserve(kRecHeader + id.size() + data.size() + 4);
    r.push_back(op);
    le16(r, uint16_t(id.size()));
    le32(r, uint32_t(data.size()));
    r.insert(r.end(), id.begin(), id.end());
    r.insert(r.end(), data.begin(), data.end());
    le32(r, crc32(r.data(), r.size()));
    return r;
}

std::vector<uint8_t> file_header() {
    std::vector<uint8_t> h(kMagic, kMagic + 4);
    le32(h, kVersion);
    return h;
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc) {
    static uint32_t table[256];
    static bool init = [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return true;
    }();
    (void)init;
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::string make_store_key(const std::string& subject, uint8_t finger) {
    if (finger == 0) return subject;
    return subject + '\x1F' + std::to_string(int(finger));
}

std::pair<std::string, uint8_t> split_store_key(const std::string& key) {
    auto sep = key.rfind('\x1F');
    if (sep == std::string::npos) return {key, uint8_t(0)};
    const std::string pos = key.substr(sep + 1);
    if (pos.empty() || pos.size() > 2 || pos.find_first_not_of("0123456789") != std::string::npos)
        throw Error("store: malformed finger position in key");
    const int f = std::stoi(pos);
    if (f > 10) throw Error("store: finger position out of range in key");
    return {key.substr(0, sep), uint8_t(f)};
}

bool valid_subject_id(const std::string& id) {
    if (id.empty() || id.size() > 256) return false;
    for (unsigned char c : id)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

TemplateStore::TemplateStore(std::string path) : path_(std::move(path)) { open_append(); }

TemplateStore::~TemplateStore() {
    if (f_) std::fclose(f_);
}

void TemplateStore::open_append() {
    f_ = std::fopen(path_.c_str(), "ab+");
    if (!f_) throw Error("store: cannot open " + path_);
    std::fseek(f_, 0, SEEK_END);
    if (std::ftell(f_) == 0) {
        auto h = file_header();
        std::fwrite(h.data(), 1, h.size(), f_);
        std::fflush(f_);
    }
}

size_t TemplateStore::replay(const PutFn& on_put, const EraseFn& on_erase, std::string* warning) {
    std::lock_guard<std::mutex> lk(mu_);
    std::fflush(f_);
    std::ifstream in(path_, std::ios::binary);
    if (!in) throw Error("store: cannot read " + path_);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), {});
    if (buf.size() < 8 || std::memcmp(buf.data(), kMagic, 4) != 0) throw Error("store: " + path_ + " is not an fpx store");
    if (rd32(&buf[4]) != kVersion) throw Error("store: unsupported version");
    size_t pos = 8, n = 0;
    std::vector<uint8_t> data;
    while (pos < buf.size()) {
        if (pos + kRecHeader > buf.size()) break;
        const uint8_t op = buf[pos];
        const size_t id_len = size_t(buf[pos + 1] | (buf[pos + 2] << 8));
        const size_t data_len = rd32(&buf[pos + 3]);
        const size_t total = kRecHeader + id_len + data_len + 4;
        if (pos + total > buf.size() || (op != kOpPut && op != kOpErase)) break;
        const uint32_t crc = rd32(&buf[pos + total - 4]);
        if (crc32(&buf[pos], total - 4) != crc) break;
        std::string id(reinterpret_cast<const char*>(&buf[pos + kRecHeader]), id_len);
        if (op == kOpPut) {
            data.assign(buf.begin() + long(pos + kRecHeader + id_len), buf.begin() + long(pos + kRecHeader + id_len + data_len));
            on_put(id, data);
        } else {
            on_erase(id);
        }
        pos += total;
        ++n;
    }
    if (pos != buf.size()) {
        if (warning)
            *warning = "store: discarded " + std::to_string(buf.size() - pos) + " trailing bytes (torn/corrupt write)";
        // Truncate the bad tail so that new appends stay reachable.
        std::fclose(f_);
        f_ = nullptr;
        std::filesystem::resize_file(path_, pos);
        f_ = std::fopen(path_.c_str(), "ab+");
        if (!f_) throw Error("store: cannot reopen " + path_);
    }
    return n;
}

void TemplateStore::append(uint8_t op, const std::string& id, const std::vector<uint8_t>& data) {
    auto rec = encode_record(op, id, data);
    std::lock_guard<std::mutex> lk(mu_);
    if (std::fwrite(rec.data(), 1, rec.size(), f_) != rec.size()) throw Error("store: write failed");
    std::fflush(f_);
}

void TemplateStore::put(const std::string& id, const std::vector<uint8_t>& record) { append(kOpPut, id, record); }
void TemplateStore::erase(const std::string& id) { append(kOpErase, id, {}); }

void TemplateStore::sync() {
    std::lock_guard<std::mutex> lk(mu_);
    std::fflush(f_);
    fpx_fsync(fpx_fileno(f_));
}

void TemplateStore::compact() {
    // Collect live records in first-insertion order.
    std::vector<std::string> order;
    std::unordered_map<std::string, std::vector<uint8_t>> live;
    replay(
        [&](const std::string& id, const std::vector<uint8_t>& d) {
            if (!live.count(id)) order.push_back(id);
            live[id] = d;
        },
        [&](const std::string& id) { live.erase(id); });
    std::lock_guard<std::mutex> lk(mu_);
    const std::string tmp = path_ + ".compact";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        auto h = file_header();
        out.write(reinterpret_cast<const char*>(h.data()), std::streamsize(h.size()));
        for (const auto& id : order) {
            auto it = live.find(id);
            if (it == live.end()) continue;
            auto rec = encode_record(kOpPut, id, it->second);
            out.write(reinterpret_cast<const char*>(rec.data()), std::streamsize(rec.size()));
        }
        if (!out) throw Error("store: compaction write failed");
    }
    std::fclose(f_);
    f_ = nullptr;
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) throw Error("store: rename failed during compaction");
    f_ = std::fopen(path_.c_str(), "ab+");
    if (!f_) throw Error("store: cannot reopen " + path_);
}

}  // namespace fpx
