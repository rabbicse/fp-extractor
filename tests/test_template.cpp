#include <doctest/doctest.h>

#include "fpx/image.hpp"
#include "fpx/synthetic.hpp"
#include "fpx/template.hpp"

using namespace fpx;

TEST_CASE("ISO and ANSI serialisation round-trip and auto-detection") {
    FingerTemplate t = synthesize_template(7);
    t.finger_position = 2;
    for (auto fmt : {TemplateFormat::Iso19794_2_2005, TemplateFormat::Ansi378_2004}) {
        auto bytes = serialize_template(t, fmt);
        CHECK(looks_like_template(bytes));
        CHECK(detect_template_format(bytes) == fmt);
        FingerTemplate back = parse_template(bytes);
        CHECK(back.format == fmt);
        CHECK(back.width == t.width);
        CHECK(back.finger_position == 2);
        REQUIRE(back.minutiae.size() == t.minutiae.size());
        for (size_t i = 0; i < t.minutiae.size(); ++i) {
            CHECK(back.minutiae[i].x == t.minutiae[i].x);
            CHECK(back.minutiae[i].y == t.minutiae[i].y);
            CHECK(back.minutiae[i].type == t.minutiae[i].type);
            // ANSI stores 2-degree units: up to ~1 ISO unit of rounding
            int d = std::abs(int(back.minutiae[i].angle) - int(t.minutiae[i].angle));
            CHECK(std::min(d, 256 - d) <= (fmt == TemplateFormat::Ansi378_2004 ? 1 : 0));
        }
    }
}

TEST_CASE("Record sizes follow the standards") {
    FingerTemplate t = synthesize_template(1);
    const size_t n = t.minutiae.size();
    CHECK(serialize_template(t, TemplateFormat::Iso19794_2_2005).size() == 24 + 4 + 6 * n + 2);
    CHECK(serialize_template(t, TemplateFormat::Ansi378_2004).size() == 26 + 4 + 6 * n + 2);
}

TEST_CASE("ANSI <-> ISO conversion preserves minutiae") {
    FingerTemplate t = synthesize_template(9);
    auto ansi = serialize_template(t, TemplateFormat::Ansi378_2004);
    auto iso = serialize_template(parse_template(ansi), TemplateFormat::Iso19794_2_2005);
    CHECK(parse_template(iso).minutiae.size() == t.minutiae.size());
}

TEST_CASE("Bad templates throw") {
    CHECK_THROWS_AS(parse_template(std::vector<uint8_t>{'F', 'M', 'R'}), Error);
    auto bytes = serialize_template(synthesize_template(2), TemplateFormat::Iso19794_2_2005);
    bytes.resize(40);
    CHECK_THROWS_AS(parse_template(bytes), Error);
    CHECK(parse_template_format("MINEX") == TemplateFormat::Ansi378_2004);
    CHECK_THROWS_AS(parse_template_format("xml"), Error);
}
