#include <doctest/doctest.h>

#include "fpx/image.hpp"
#include "fpx/synthetic.hpp"
#include "fpx/wsq.hpp"

using namespace fpx;

static GrayImage gradient(int w, int h) {
    GrayImage img(w, h, 500);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) img.at(x, y) = uint8_t((x * 3 + y * 7) & 255);
    return img;
}

TEST_CASE("BMP 8-bit round trip keeps pixels, orientation and dpi") {
    GrayImage img = gradient(37, 23);  // odd width -> exercises row padding
    img.dpi = 508;
    auto bytes = encode_bmp(img);
    CHECK(detect_image_format(bytes) == ImageFormat::Bmp);
    GrayImage back = decode_bmp(bytes);
    CHECK(back.width == 37);
    CHECK(back.height == 23);
    CHECK(back.dpi == 508);
    CHECK(back.pixels == img.pixels);
}

TEST_CASE("BMP 24-bit bottom-up and top-down are decoded to the same image") {
    const int w = 5, h = 3;
    auto make = [&](bool top_down) {
        const int stride = (w * 3 + 3) & ~3;
        std::vector<uint8_t> b(54 + stride * h, 0);
        b[0] = 'B'; b[1] = 'M';
        auto w32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) b[o + i] = uint8_t(v >> (8 * i)); };
        w32(2, uint32_t(b.size())); w32(10, 54); w32(14, 40); w32(18, w);
        w32(22, top_down ? uint32_t(-h) : uint32_t(h));
        b[26] = 1; b[28] = 24;
        for (int y = 0; y < h; ++y) {
            int row = top_down ? y : h - 1 - y;
            for (int x = 0; x < w; ++x) {
                uint8_t v = uint8_t(y * 50 + x * 10);
                size_t o = 54 + size_t(row) * stride + size_t(x) * 3;
                b[o] = b[o + 1] = b[o + 2] = v;
            }
        }
        return b;
    };
    GrayImage a = decode_bmp(make(false)), t = decode_bmp(make(true));
    CHECK(a.pixels == t.pixels);
    CHECK(a.at(4, 2) == 2 * 50 + 4 * 10);
    CHECK(a.dpi == 500);  // header had 0 ppm -> default
}

TEST_CASE("The original bug: BMP header bytes must not be treated as pixels") {
    GrayImage img = gradient(64, 48);
    auto bytes = encode_bmp(img);
    GrayImage back = decode_image(bytes);
    CHECK(back.pixels.size() == size_t(64 * 48));
    CHECK(back.pixels[0] == img.pixels[0]);  // first pixel is pixel data, not 'B'
}

TEST_CASE("PGM round trip") {
    GrayImage img = gradient(20, 10);
    GrayImage back = decode_pgm(encode_pgm(img));
    CHECK(back.pixels == img.pixels);
}

TEST_CASE("Malformed images are rejected with fpx::Error") {
    CHECK_THROWS_AS(decode_image({1, 2, 3}), Error);
    CHECK_THROWS_AS(decode_bmp({'B', 'M', 0, 0}), Error);
    std::vector<uint8_t> truncated = encode_bmp(gradient(40, 40));
    truncated.resize(truncated.size() / 2);
    CHECK_THROWS_AS(decode_bmp(truncated), Error);
}

TEST_CASE("WSQ compresses a fingerprint and decodes with small error") {
    GrayImage img = synthesize_image(3, 0);
    auto wsq = wsq_encode(img, 0.75f);
    CHECK(detect_image_format(wsq) == ImageFormat::Wsq);
    CHECK(wsq.size() < img.pixels.size() / 5);  // >5:1 compression
    GrayImage back = wsq_decode(wsq);
    REQUIRE(back.width == img.width);
    REQUIRE(back.height == img.height);
    CHECK(back.dpi == 500);
    double err = 0;
    for (size_t i = 0; i < img.pixels.size(); ++i) err += std::abs(int(img.pixels[i]) - int(back.pixels[i]));
    err /= double(img.pixels.size());
    CHECK(err < 20.0);  // mean absolute error in grey levels (synthetic image is noisy)
}

TEST_CASE("fit_center crops and pads around the centre") {
    GrayImage img = gradient(10, 10);
    GrayImage c = fit_center(img, 4, 4);
    CHECK(c.at(0, 0) == img.at(3, 3));
    GrayImage p = fit_center(img, 14, 10);
    CHECK(p.at(0, 0) == 255);
    CHECK(p.at(2, 0) == img.at(0, 0));
}
