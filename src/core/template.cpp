// SPDX-License-Identifier: MIT
#include "fpx/template.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "fpx/image.hpp"  // fpx::Error

namespace fpx {
namespace {

uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    put16(v, uint16_t(x >> 16));
    put16(v, uint16_t(x));
}

constexpr size_t kIsoHeader = 24;
constexpr size_t kAnsiHeader = 26;
constexpr size_t kAnsiHeaderLong = 30;  // 6-byte record length variant

bool has_magic(const uint8_t* d, size_t n) {
    return n >= 8 && d[0] == 'F' && d[1] == 'M' && d[2] == 'R' && d[3] == 0 && d[4] == ' ' && d[5] == '2' &&
           d[6] == '0' && d[7] == 0;
}

// Does a single-view record with header length `hdr` exactly fit `n` bytes?
bool fits(const uint8_t* d, size_t n, size_t hdr) {
    if (n < hdr + 4) return false;
    size_t nm = d[hdr + 3];
    size_t need = hdr + 4 + 6 * nm + 2;
    return need <= n;
}

}  // namespace

const char* to_string(TemplateFormat f) {
    return f == TemplateFormat::Ansi378_2004 ? "ANSI-378-2004" : "ISO-19794-2-2005";
}

TemplateFormat parse_template_format(const std::string& s0) {
    std::string s = s0;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    if (s == "ansi" || s == "minex" || s == "ansi378" || s == "ansi-378" || s == "ansi378-2004")
        return TemplateFormat::Ansi378_2004;
    if (s == "iso" || s == "iso19794" || s == "iso19794-2" || s == "iso-19794-2" || s == "iso19794-2-2005")
        return TemplateFormat::Iso19794_2_2005;
    throw Error("unknown template format '" + s0 + "' (use ansi|minex|iso)");
}

static TemplateFormat detect(const uint8_t* d, size_t n, size_t* header_len) {
    if (!has_magic(d, n) || n < 26) throw Error("template: missing 'FMR\\0 20\\0' header");
    const uint32_t len32 = be32(d + 8);
    const uint16_t len16 = be16(d + 8);
    if (len32 == n && fits(d, n, kIsoHeader)) {
        *header_len = kIsoHeader;
        return TemplateFormat::Iso19794_2_2005;
    }
    if (len16 == n && fits(d, n, kAnsiHeader)) {
        *header_len = kAnsiHeader;
        return TemplateFormat::Ansi378_2004;
    }
    if (len16 == 0 && n >= kAnsiHeaderLong && be32(d + 10) == n && fits(d, n, kAnsiHeaderLong)) {
        *header_len = kAnsiHeaderLong;
        return TemplateFormat::Ansi378_2004;
    }
    // Length field wrong (some SDKs pad buffers): fall back to structure.
    if (fits(d, n, kIsoHeader) && d[22] >= 1 && d[23] == 0 && !(len16 >= 26 && len16 <= n && fits(d, n, kAnsiHeader))) {
        *header_len = kIsoHeader;
        return TemplateFormat::Iso19794_2_2005;
    }
    if (fits(d, n, kAnsiHeader)) {
        *header_len = kAnsiHeader;
        return TemplateFormat::Ansi378_2004;
    }
    throw Error("template: cannot determine ANSI/ISO layout (truncated?)");
}

TemplateFormat detect_template_format(const std::vector<uint8_t>& bytes) {
    size_t h;
    return detect(bytes.data(), bytes.size(), &h);
}

bool looks_like_template(const std::vector<uint8_t>& bytes) { return has_magic(bytes.data(), bytes.size()); }

FingerTemplate parse_template(const uint8_t* d, size_t n) {
    size_t hdr = 0;
    FingerTemplate t;
    t.format = detect(d, n, &hdr);
    const size_t off = hdr == kAnsiHeaderLong ? 4 : 0;  // ANSI long-length shift
    if (t.format == TemplateFormat::Ansi378_2004) {
        t.cbeff_product_id = be32(d + 10 + off);
        t.width = be16(d + 16 + off);
        t.height = be16(d + 18 + off);
        t.xres = be16(d + 20 + off);
        t.yres = be16(d + 22 + off);
        if (d[24 + off] == 0) throw Error("template: record contains no finger view");
    } else {
        t.width = be16(d + 14);
        t.height = be16(d + 16);
        t.xres = be16(d + 18);
        t.yres = be16(d + 20);
        if (d[22] == 0) throw Error("template: record contains no finger view");
    }
    if (t.xres == 0) t.xres = 197;
    if (t.yres == 0) t.yres = t.xres;
    const uint8_t* v = d + hdr;
    t.finger_position = v[0];
    t.impression_type = v[1] & 0x0F;
    t.finger_quality = v[2];
    const size_t nm = v[3];
    const uint8_t* m = v + 4;
    t.minutiae.reserve(nm);
    for (size_t i = 0; i < nm; ++i, m += 6) {
        Minutia mi;
        mi.type = MinutiaType((m[0] >> 6) & 3);
        if (mi.type != MinutiaType::Ending && mi.type != MinutiaType::Bifurcation) mi.type = MinutiaType::Other;
        mi.x = uint16_t(be16(m) & 0x3FFF);
        mi.y = uint16_t(be16(m + 2) & 0x3FFF);
        if (t.format == TemplateFormat::Ansi378_2004) {
            // ANSI: 2-degree units, 0..179
            mi.angle = uint8_t(std::lround((m[4] % 180) * 512.0 / 360.0) & 0xFF);
        } else {
            mi.angle = m[4];
        }
        mi.quality = m[5];
        t.minutiae.push_back(mi);
    }
    return t;
}

FingerTemplate parse_template(const std::vector<uint8_t>& bytes) { return parse_template(bytes.data(), bytes.size()); }

std::vector<uint8_t> serialize_template(const FingerTemplate& t, TemplateFormat format) {
    const size_t nm = std::min<size_t>(t.minutiae.size(), 255);
    std::vector<uint8_t> v;
    const bool ansi = format == TemplateFormat::Ansi378_2004;
    const size_t total = (ansi ? kAnsiHeader : kIsoHeader) + 4 + 6 * nm + 2;
    v.reserve(total);
    const uint8_t magic[8] = {'F', 'M', 'R', 0, ' ', '2', '0', 0};
    v.insert(v.end(), magic, magic + 8);
    if (ansi) {
        put16(v, uint16_t(total));
        put32(v, t.cbeff_product_id);
        put16(v, 0);  // capture equipment
    } else {
        put32(v, uint32_t(total));
        put16(v, 0);
    }
    put16(v, t.width);
    put16(v, t.height);
    put16(v, t.xres);
    put16(v, t.yres);
    v.push_back(1);  // one finger view
    v.push_back(0);
    v.push_back(t.finger_position);
    v.push_back(uint8_t(t.impression_type & 0x0F));  // view number 0
    v.push_back(t.finger_quality);
    v.push_back(uint8_t(nm));
    for (size_t i = 0; i < nm; ++i) {
        const Minutia& m = t.minutiae[i];
        put16(v, uint16_t((uint16_t(m.type) << 14) | (m.x & 0x3FFF)));
        put16(v, uint16_t(m.y & 0x3FFF));
        if (ansi)
            v.push_back(uint8_t(std::lround(m.angle * 360.0 / 512.0) % 180));
        else
            v.push_back(m.angle);
        v.push_back(m.quality);
    }
    put16(v, 0);  // extended data block length
    return v;
}

}  // namespace fpx
