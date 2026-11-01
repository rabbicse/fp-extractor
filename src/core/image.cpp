// SPDX-License-Identifier: MIT
#include "fpx/image.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#include "fpx/wsq.hpp"

namespace fpx {
namespace {

uint16_t rd16le(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void wr16le(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}
void wr32le(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

inline uint8_t luma(uint8_t r, uint8_t g, uint8_t b) {
    return uint8_t((299u * r + 587u * g + 114u * b + 500u) / 1000u);
}

// Scanner drivers often write 0 or 96/72 dpi into headers. Values outside the
// range a fingerprint sensor can have are treated as "unknown".
int sane_dpi(double dpi, int fallback) {
    if (dpi >= 250 && dpi <= 1300) return int(std::lround(dpi));
    return fallback;
}

}  // namespace

const char* to_string(ImageFormat f) {
    switch (f) {
        case ImageFormat::Bmp: return "BMP";
        case ImageFormat::Pgm: return "PGM";
        case ImageFormat::Tiff: return "TIFF";
        case ImageFormat::Wsq: return "WSQ";
        default: return "unknown";
    }
}

ImageFormat detect_image_format(const std::vector<uint8_t>& b) {
    if (b.size() >= 2 && b[0] == 'B' && b[1] == 'M') return ImageFormat::Bmp;
    if (b.size() >= 2 && b[0] == 'P' && b[1] == '5') return ImageFormat::Pgm;
    if (b.size() >= 4 && ((b[0] == 'I' && b[1] == 'I' && b[2] == 42 && b[3] == 0) ||
                          (b[0] == 'M' && b[1] == 'M' && b[2] == 0 && b[3] == 42)))
        return ImageFormat::Tiff;
    if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xA0) return ImageFormat::Wsq;  // SOI marker
    return ImageFormat::Unknown;
}

ImageFormat image_format_from_extension(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return ImageFormat::Unknown;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == "bmp") return ImageFormat::Bmp;
    if (ext == "pgm") return ImageFormat::Pgm;
    if (ext == "tif" || ext == "tiff") return ImageFormat::Tiff;
    if (ext == "wsq") return ImageFormat::Wsq;
    return ImageFormat::Unknown;
}

// ---------------------------------------------------------------------------
// BMP
// ---------------------------------------------------------------------------
GrayImage decode_bmp(const std::vector<uint8_t>& b, int default_dpi) {
    if (b.size() < 26 || b[0] != 'B' || b[1] != 'M') throw Error("BMP: bad signature");
    const uint32_t data_off = rd32le(&b[10]);
    const uint32_t dib = rd32le(&b[14]);
    int32_t w, h;
    uint16_t bpp;
    uint32_t compression = 0, clr_used = 0;
    int32_t xppm = 0;
    size_t pal_off = 14 + dib, pal_entry = 4;
    if (dib == 12) {  // BITMAPCOREHEADER
        w = rd16le(&b[18]);
        h = int16_t(rd16le(&b[20]));
        bpp = rd16le(&b[24]);
        pal_entry = 3;
    } else if (dib >= 40 && b.size() >= 54) {
        w = int32_t(rd32le(&b[18]));
        h = int32_t(rd32le(&b[22]));
        bpp = rd16le(&b[28]);
        compression = rd32le(&b[30]);
        xppm = int32_t(rd32le(&b[38]));
        clr_used = rd32le(&b[46]);
        if (dib == 40 && compression == 3) pal_off += 12;  // BI_BITFIELDS masks after header
    } else {
        throw Error("BMP: unsupported header size " + std::to_string(dib));
    }
    if (compression != 0 && !(compression == 3 && (bpp == 32 || bpp == 16)))
        throw Error("BMP: compressed bitmaps are not supported");
    if (w <= 0 || h == 0 || w > 10000 || std::abs(h) > 10000) throw Error("BMP: bad dimensions");
    const bool top_down = h < 0;
    h = std::abs(h);
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
        throw Error("BMP: unsupported bit depth " + std::to_string(bpp));

    std::vector<uint8_t> pal;  // gray value per index
    if (bpp <= 8) {
        size_t n = clr_used ? clr_used : (1u << bpp);
        pal.assign(256, 0);
        for (size_t i = 0; i < n && i < 256; ++i) {
            size_t o = pal_off + i * pal_entry;
            if (o + 3 > b.size()) throw Error("BMP: truncated palette");
            pal[i] = luma(b[o + 2], b[o + 1], b[o]);
        }
    }
    const size_t stride = ((size_t(bpp) * w + 31) / 32) * 4;
    if (data_off + stride * h > b.size()) throw Error("BMP: truncated pixel data");

    GrayImage img(w, h, sane_dpi(xppm * 0.0254, default_dpi));
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &b[data_off + stride * (top_down ? y : (h - 1 - y))];
        uint8_t* out = &img.pixels[size_t(y) * w];
        switch (bpp) {
            case 8:
                for (int x = 0; x < w; ++x) out[x] = pal[row[x]];
                break;
            case 4:
                for (int x = 0; x < w; ++x) out[x] = pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0xF];
                break;
            case 1:
                for (int x = 0; x < w; ++x) out[x] = pal[(row[x >> 3] >> (7 - (x & 7))) & 1];
                break;
            case 24:
                for (int x = 0; x < w; ++x) out[x] = luma(row[3 * x + 2], row[3 * x + 1], row[3 * x]);
                break;
            case 32:
                for (int x = 0; x < w; ++x) out[x] = luma(row[4 * x + 2], row[4 * x + 1], row[4 * x]);
                break;
        }
    }
    return img;
}

std::vector<uint8_t> encode_bmp(const GrayImage& img) {
    if (img.empty()) throw Error("BMP: empty image");
    const uint32_t stride = ((8u * img.width + 31) / 32) * 4;
    const uint32_t data_off = 14 + 40 + 256 * 4;
    const uint32_t file_size = data_off + stride * img.height;
    const uint32_t ppm = uint32_t(std::lround(img.dpi / 0.0254));
    std::vector<uint8_t> v;
    v.reserve(file_size);
    v.push_back('B');
    v.push_back('M');
    wr32le(v, file_size);
    wr32le(v, 0);
    wr32le(v, data_off);
    wr32le(v, 40);
    wr32le(v, uint32_t(img.width));
    wr32le(v, uint32_t(img.height));  // positive = bottom-up
    wr16le(v, 1);
    wr16le(v, 8);
    wr32le(v, 0);
    wr32le(v, stride * img.height);
    wr32le(v, ppm);
    wr32le(v, ppm);
    wr32le(v, 256);
    wr32le(v, 0);
    for (int i = 0; i < 256; ++i) {
        v.push_back(uint8_t(i));
        v.push_back(uint8_t(i));
        v.push_back(uint8_t(i));
        v.push_back(0);
    }
    for (int y = img.height - 1; y >= 0; --y) {
        const uint8_t* row = &img.pixels[size_t(y) * img.width];
        v.insert(v.end(), row, row + img.width);
        for (uint32_t p = img.width; p < stride; ++p) v.push_back(0);
    }
    return v;
}

// ---------------------------------------------------------------------------
// PGM (binary P5)
// ---------------------------------------------------------------------------
GrayImage decode_pgm(const std::vector<uint8_t>& b, int default_dpi) {
    size_t pos = 2;
    auto next_int = [&]() -> long {
        for (;;) {
            while (pos < b.size() && std::isspace(b[pos])) ++pos;
            if (pos < b.size() && b[pos] == '#') {
                while (pos < b.size() && b[pos] != '\n') ++pos;
                continue;
            }
            break;
        }
        if (pos >= b.size() || !std::isdigit(b[pos])) throw Error("PGM: bad header");
        long v = 0;
        while (pos < b.size() && std::isdigit(b[pos])) v = v * 10 + (b[pos++] - '0');
        return v;
    };
    if (b.size() < 2 || b[0] != 'P' || b[1] != '5') throw Error("PGM: only binary P5 is supported");
    long w = next_int(), h = next_int(), maxv = next_int();
    ++pos;  // single whitespace
    if (w <= 0 || h <= 0 || w > 10000 || h > 10000 || maxv <= 0 || maxv > 65535) throw Error("PGM: bad header");
    const size_t bps = maxv > 255 ? 2 : 1;
    if (pos + size_t(w) * h * bps > b.size()) throw Error("PGM: truncated");
    GrayImage img(int(w), int(h), default_dpi);
    for (size_t i = 0; i < img.pixels.size(); ++i) {
        unsigned v = bps == 1 ? b[pos + i] : unsigned((b[pos + 2 * i] << 8) | b[pos + 2 * i + 1]);
        img.pixels[i] = uint8_t(v * 255 / unsigned(maxv));
    }
    return img;
}

std::vector<uint8_t> encode_pgm(const GrayImage& img) {
    std::string hdr = "P5\n# dpi " + std::to_string(img.dpi) + "\n" + std::to_string(img.width) + " " +
                      std::to_string(img.height) + "\n255\n";
    std::vector<uint8_t> v(hdr.begin(), hdr.end());
    v.insert(v.end(), img.pixels.begin(), img.pixels.end());
    return v;
}

// ---------------------------------------------------------------------------
// TIFF (baseline, uncompressed, 8-bit gray or RGB) - e.g. FVC databases
// ---------------------------------------------------------------------------
GrayImage decode_tiff(const std::vector<uint8_t>& b, int default_dpi) {
    if (b.size() < 8) throw Error("TIFF: too short");
    const bool le = b[0] == 'I';
    auto u16 = [&](size_t o) -> uint32_t {
        if (o + 2 > b.size()) throw Error("TIFF: truncated");
        return le ? uint32_t(b[o] | (b[o + 1] << 8)) : uint32_t((b[o] << 8) | b[o + 1]);
    };
    auto u32 = [&](size_t o) -> uint32_t {
        if (o + 4 > b.size()) throw Error("TIFF: truncated");
        return le ? rd32le(&b[o])
                  : (uint32_t(b[o]) << 24) | (uint32_t(b[o + 1]) << 16) | (uint32_t(b[o + 2]) << 8) | b[o + 3];
    };
    const uint32_t ifd = u32(4);
    const uint32_t n = u16(ifd);
    uint32_t w = 0, h = 0, bps = 8, comp = 1, photometric = 1, spp = 1, rps = 0xFFFFFFFF, res_unit = 2;
    double xres = 0;
    std::vector<uint32_t> strip_off, strip_cnt;
    auto values = [&](size_t e, std::vector<uint32_t>& out) {
        uint32_t type = u16(e + 2), cnt = u32(e + 4);
        size_t sz = type == 3 ? 2 : 4;
        size_t base = cnt * sz <= 4 ? e + 8 : u32(e + 8);
        out.clear();
        for (uint32_t i = 0; i < cnt; ++i) out.push_back(sz == 2 ? u16(base + i * 2) : u32(base + i * 4));
    };
    auto scalar = [&](size_t e) -> uint32_t { return u16(e + 2) == 3 ? u16(e + 8) : u32(e + 8); };
    for (uint32_t i = 0; i < n; ++i) {
        size_t e = ifd + 2 + i * 12;
        switch (u16(e)) {
            case 256: w = scalar(e); break;
            case 257: h = scalar(e); break;
            case 258: bps = u16(u32(e + 4) == 1 ? e + 8 : u32(e + 8)); break;
            case 259: comp = scalar(e); break;
            case 262: photometric = scalar(e); break;
            case 273: values(e, strip_off); break;
            case 277: spp = scalar(e); break;
            case 278: rps = scalar(e); break;
            case 279: values(e, strip_cnt); break;
            case 282: {
                uint32_t o = u32(e + 8), den = u32(o + 4);
                xres = den ? double(u32(o)) / den : 0;
                break;
            }
            case 296: res_unit = scalar(e); break;
            default: break;
        }
    }
    (void)rps;
    if (comp != 1) throw Error("TIFF: only uncompressed TIFF is supported (convert with `convert in.tif -compress none out.tif`)");
    if (bps != 8 || (spp != 1 && spp != 3 && spp != 4)) throw Error("TIFF: only 8-bit gray/RGB supported");
    if (!w || !h || w > 10000 || h > 10000 || strip_off.empty()) throw Error("TIFF: bad header");
    std::vector<uint8_t> raw;
    raw.reserve(size_t(w) * h * spp);
    for (size_t s = 0; s < strip_off.size(); ++s) {
        size_t cnt = s < strip_cnt.size() ? strip_cnt[s] : size_t(w) * h * spp;
        if (strip_off[s] + cnt > b.size()) throw Error("TIFF: truncated strip");
        raw.insert(raw.end(), b.begin() + strip_off[s], b.begin() + strip_off[s] + cnt);
    }
    if (raw.size() < size_t(w) * h * spp) throw Error("TIFF: not enough pixel data");
    double dpi = res_unit == 3 ? xres * 2.54 : xres;
    GrayImage img(int(w), int(h), sane_dpi(dpi, default_dpi));
    for (size_t i = 0; i < img.pixels.size(); ++i) {
        uint8_t v = spp == 1 ? raw[i] : luma(raw[i * spp], raw[i * spp + 1], raw[i * spp + 2]);
        img.pixels[i] = photometric == 0 ? uint8_t(255 - v) : v;
    }
    return img;
}

GrayImage from_raw(const uint8_t* data, size_t size, int width, int height, int dpi) {
    if (width <= 0 || height <= 0 || size < size_t(width) * height) throw Error("raw: size mismatch");
    GrayImage img(width, height, dpi);
    std::memcpy(img.pixels.data(), data, img.pixels.size());
    return img;
}

GrayImage decode_image(const std::vector<uint8_t>& bytes, int default_dpi) {
    switch (detect_image_format(bytes)) {
        case ImageFormat::Bmp: return decode_bmp(bytes, default_dpi);
        case ImageFormat::Pgm: return decode_pgm(bytes, default_dpi);
        case ImageFormat::Tiff: return decode_tiff(bytes, default_dpi);
        case ImageFormat::Wsq: return wsq_decode(bytes, default_dpi);
        default: throw Error("unrecognised image format (supported: BMP, PGM, TIFF, WSQ)");
    }
}

std::vector<uint8_t> encode_image(const GrayImage& img, ImageFormat fmt, float wsq_bitrate) {
    switch (fmt) {
        case ImageFormat::Bmp: return encode_bmp(img);
        case ImageFormat::Pgm: return encode_pgm(img);
        case ImageFormat::Wsq: return wsq_encode(img, wsq_bitrate);
        default: throw Error("cannot encode to this format (use .bmp, .pgm or .wsq)");
    }
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Error("cannot open " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

void write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw Error("cannot write " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!f) throw Error("write failed: " + path);
}

GrayImage load_image(const std::string& path, int default_dpi) { return decode_image(read_file(path), default_dpi); }

void save_image(const std::string& path, const GrayImage& img, float wsq_bitrate) {
    write_file(path, encode_image(img, image_format_from_extension(path), wsq_bitrate));
}

GrayImage fit_center(const GrayImage& img, int w, int h) {
    GrayImage out(w, h, img.dpi, 255);
    const int ox = (img.width - w) / 2, oy = (img.height - h) / 2;
    for (int y = 0; y < h; ++y) {
        int sy = y + oy;
        if (sy < 0 || sy >= img.height) continue;
        for (int x = 0; x < w; ++x) {
            int sx = x + ox;
            if (sx >= 0 && sx < img.width) out.at(x, y) = img.at(sx, sy);
        }
    }
    return out;
}

}  // namespace fpx
