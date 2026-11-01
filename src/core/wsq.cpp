// SPDX-License-Identifier: MIT
#include "fpx/wsq.hpp"

#include <cstdlib>
#include <mutex>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wregister"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
extern "C" {
#define register
#include <wsq.h>
#undef register
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fpx {
namespace {
// NBIS keeps codec state in globals -> serialise access.
std::mutex g_wsq_mutex;
}  // namespace

std::vector<uint8_t> wsq_encode(const GrayImage& img, float bitrate) {
    if (img.empty()) throw Error("WSQ: empty image");
    if (img.width < 32 || img.height < 32) throw Error("WSQ: image too small");
    if (bitrate <= 0.0f || bitrate > 8.0f) throw Error("WSQ: bitrate must be in (0, 8]");
    std::vector<uint8_t> in = img.pixels;  // NBIS takes a non-const pointer
    unsigned char* out = nullptr;
    int out_len = 0;
    int rc;
    {
        std::lock_guard<std::mutex> lk(g_wsq_mutex);
        rc = wsq_encode_mem(&out, &out_len, bitrate, in.data(), img.width, img.height, 8, img.dpi, nullptr);
    }
    if (rc != 0 || !out) {
        std::free(out);
        throw Error("WSQ: encoding failed (code " + std::to_string(rc) + ")");
    }
    std::vector<uint8_t> v(out, out + out_len);
    std::free(out);
    return v;
}

GrayImage wsq_decode(const std::vector<uint8_t>& wsq, int default_dpi) {
    if (wsq.size() < 2 || wsq[0] != 0xFF || wsq[1] != 0xA0) throw Error("WSQ: bad signature");
    std::vector<uint8_t> in = wsq;
    unsigned char* out = nullptr;
    int w = 0, h = 0, d = 0, ppi = -1, lossy = 0;
    int rc;
    {
        std::lock_guard<std::mutex> lk(g_wsq_mutex);
        rc = wsq_decode_mem(&out, &w, &h, &d, &ppi, &lossy, in.data(), int(in.size()));
    }
    if (rc != 0 || !out) {
        std::free(out);
        throw Error("WSQ: decoding failed (code " + std::to_string(rc) + ")");
    }
    GrayImage img(w, h, ppi > 0 ? ppi : default_dpi);
    std::copy(out, out + size_t(w) * h, img.pixels.begin());
    std::free(out);
    return img;
}

}  // namespace fpx
