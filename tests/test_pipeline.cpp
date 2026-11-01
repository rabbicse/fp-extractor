// End-to-end with the real FingerJet extractor:
// synthetic scanner image -> BMP -> WSQ -> template -> match / identify.
#include <doctest/doctest.h>

#include "fpx/fpx.hpp"

using namespace fpx;

TEST_CASE("Full pipeline: BMP -> WSQ -> ISO/ANSI -> match") {
    std::vector<FingerTemplate> t;
    for (uint32_t f = 1; f <= 3; ++f)
        for (uint32_t imp = 0; imp < 2; ++imp) {
            GrayImage img = decode_image(encode_bmp(synthesize_image(f, imp)));  // through the BMP codec
            GrayImage via_wsq = wsq_decode(wsq_encode(img));                    // and through WSQ
            ExtractOptions o;
            o.format = imp == 0 ? TemplateFormat::Iso19794_2_2005 : TemplateFormat::Ansi378_2004;
            ExtractResult r = extract(via_wsq, o);
            CHECK(r.parsed.format == o.format);
            CHECK(r.parsed.minutiae.size() >= 15);
            CHECK(r.parsed.xres == 197);  // 500 dpi in px/cm
            t.push_back(parse_template(r.record));
        }
    Matcher m;
    for (size_t i = 0; i < t.size(); ++i)
        for (size_t j = i + 1; j < t.size(); ++j) {
            float s = m.match(t[i], t[j]);
            if (i / 2 == j / 2)
                CHECK_MESSAGE(s >= kIdentifyThreshold, "genuine pair " << i << "," << j << " score " << s);
            else
                CHECK_MESSAGE(s < kVerifyThreshold, "impostor pair " << i << "," << j << " score " << s);
        }
}

TEST_CASE("Large-platen image: finger off-centre is still found, coordinates preserved") {
    // 800 x 750 canvas (e.g. a 10-print flat scanner) with the finger in the top-left.
    GrayImage finger = synthesize_image(4, 0);
    GrayImage big(800, 750, 500, 235);
    for (int y = 0; y < finger.height; ++y)
        for (int x = 0; x < finger.width; ++x) big.at(x + 20, y + 30) = finger.at(x, y);
    ExtractResult r = extract(big);
    CHECK(r.parsed.width == 800);
    CHECK(r.parsed.height == 750);
    REQUIRE(r.parsed.minutiae.size() >= 15);
    for (auto& m : r.parsed.minutiae) {
        CHECK(m.x >= 20);
        CHECK(m.x < 20 + finger.width);
    }
    // Same finger extracted from the small image must match the big one.
    CHECK(Matcher().match(extract(finger).parsed, r.parsed) >= kIdentifyThreshold);

    ExtractOptions strict;
    strict.auto_fit = false;
    CHECK_THROWS_AS(extract(big, strict), Error);
    GrayImage low = finger;
    low.dpi = 200;
    CHECK_THROWS_AS(extract(low), Error);
}

TEST_CASE("Blank image is rejected with a clear error") {
    GrayImage blank(320, 400, 500, 255);
    CHECK_THROWS_WITH_AS(extract(blank), doctest::Contains("no fingerprint"), Error);
}
