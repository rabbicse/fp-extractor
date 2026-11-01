#include <doctest/doctest.h>

#include "fpx/fpx.hpp"
#include "fpx/fpx_c.h"

using namespace fpx;

TEST_CASE("C API: image -> template -> match") {
    auto bmp_a = encode_bmp(synthesize_image(8, 0));
    auto bmp_b = encode_bmp(synthesize_image(8, 1));
    unsigned char *ta = nullptr, *tb = nullptr, *wsq = nullptr, *iso = nullptr;
    size_t la = 0, lb = 0, lw = 0, li = 0;
    REQUIRE(fpx_extract_from_image(bmp_a.data(), bmp_a.size(), 500, FPX_FORMAT_ANSI378, &ta, &la) == FPX_OK);
    REQUIRE(fpx_extract_from_image(bmp_b.data(), bmp_b.size(), 500, FPX_FORMAT_ISO19794, &tb, &lb) == FPX_OK);
    float score = 0;
    REQUIRE(fpx_match(ta, la, tb, lb, &score) == FPX_OK);
    CHECK(score >= fpx_default_threshold());
    REQUIRE(fpx_image_to_wsq(bmp_a.data(), bmp_a.size(), 500, 0.75f, &wsq, &lw) == FPX_OK);
    CHECK(lw > 100);
    REQUIRE(fpx_convert_template(ta, la, FPX_FORMAT_ISO19794, &iso, &li) == FPX_OK);
    CHECK(detect_template_format(std::vector<uint8_t>(iso, iso + li)) == TemplateFormat::Iso19794_2_2005);
    fpx_free(ta);
    fpx_free(tb);
    fpx_free(wsq);
    fpx_free(iso);
}

TEST_CASE("C API: errors are reported") {
    unsigned char junk[4] = {1, 2, 3, 4};
    unsigned char* out = nullptr;
    size_t len = 0;
    CHECK(fpx_extract_from_image(junk, 4, 500, FPX_FORMAT_ISO19794, &out, &len) == FPX_E_BAD_INPUT);
    CHECK(std::string(fpx_last_error()).size() > 0);
    CHECK(fpx_extract_from_image(nullptr, 0, 500, 1, &out, &len) == FPX_E_INVALID_ARG);
    CHECK(std::string(fpx_version()) == FPX_VERSION_STRING);
}
