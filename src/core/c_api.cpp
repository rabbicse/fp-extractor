// SPDX-License-Identifier: MIT
#include "fpx/fpx_c.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "fpx/extractor.hpp"
#include "fpx/matcher.hpp"
#include "fpx/version.hpp"
#include "fpx/wsq.hpp"

namespace {
thread_local std::string g_err;

int fail(int code, const std::string& msg) {
    g_err = msg;
    return code;
}

int give(const std::vector<uint8_t>& v, unsigned char** out, size_t* len) {
    auto* p = static_cast<unsigned char*>(std::malloc(v.empty() ? 1 : v.size()));
    if (!p) return fail(FPX_E_INTERNAL, "out of memory");
    std::memcpy(p, v.data(), v.size());
    *out = p;
    *len = v.size();
    return FPX_OK;
}

fpx::TemplateFormat fmt(int f) {
    return f == FPX_FORMAT_ANSI378 ? fpx::TemplateFormat::Ansi378_2004 : fpx::TemplateFormat::Iso19794_2_2005;
}

template <class F>
int guarded(F&& f) {
    try {
        g_err.clear();
        return f();
    } catch (const fpx::Error& e) {
        std::string m = e.what();
        return fail(m.rfind("extract:", 0) == 0 ? FPX_E_EXTRACTION : FPX_E_BAD_INPUT, m);
    } catch (const std::exception& e) {
        return fail(FPX_E_INTERNAL, e.what());
    }
}
}  // namespace

extern "C" {

const char* fpx_version(void) { return FPX_VERSION_STRING; }
const char* fpx_last_error(void) { return g_err.c_str(); }
void fpx_free(void* p) { std::free(p); }

int fpx_extract_from_image(const unsigned char* image, size_t image_len, int dpi, int format,
                           unsigned char** out, size_t* out_len) {
    if (!image || !out || !out_len) return fail(FPX_E_INVALID_ARG, "null argument");
    return guarded([&] {
        auto img = fpx::decode_image(std::vector<uint8_t>(image, image + image_len), dpi > 0 ? dpi : 500);
        fpx::ExtractOptions o;
        o.format = fmt(format);
        return give(fpx::extract_template(img, o), out, out_len);
    });
}

int fpx_extract_from_raw(const unsigned char* pixels, int width, int height, int dpi, int format,
                         unsigned char** out, size_t* out_len) {
    if (!pixels || !out || !out_len || width <= 0 || height <= 0) return fail(FPX_E_INVALID_ARG, "bad argument");
    return guarded([&] {
        auto img = fpx::from_raw(pixels, size_t(width) * height, width, height, dpi > 0 ? dpi : 500);
        fpx::ExtractOptions o;
        o.format = fmt(format);
        return give(fpx::extract_template(img, o), out, out_len);
    });
}

int fpx_image_to_wsq(const unsigned char* image, size_t image_len, int dpi, float bitrate, unsigned char** out,
                     size_t* out_len) {
    if (!image || !out || !out_len) return fail(FPX_E_INVALID_ARG, "null argument");
    return guarded([&] {
        auto img = fpx::decode_image(std::vector<uint8_t>(image, image + image_len), dpi > 0 ? dpi : 500);
        return give(fpx::wsq_encode(img, bitrate > 0 ? bitrate : 0.75f), out, out_len);
    });
}

int fpx_convert_template(const unsigned char* t, size_t len, int format, unsigned char** out, size_t* out_len) {
    if (!t || !out || !out_len) return fail(FPX_E_INVALID_ARG, "null argument");
    return guarded([&] {
        auto ft = fpx::parse_template(t, len);
        return give(fpx::serialize_template(ft, fmt(format)), out, out_len);
    });
}

int fpx_match(const unsigned char* a, size_t a_len, const unsigned char* b, size_t b_len, float* score) {
    if (!a || !b || !score) return fail(FPX_E_INVALID_ARG, "null argument");
    return guarded([&] {
        static const fpx::Matcher matcher;
        *score = matcher.match(fpx::parse_template(a, a_len), fpx::parse_template(b, b_len));
        return FPX_OK;
    });
}

float fpx_default_threshold(void) { return fpx::kDefaultThreshold; }

}  // extern "C"
