#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

#include "fpx/gallery.hpp"
#include "fpx/store.hpp"
#include "fpx/synthetic.hpp"

using namespace fpx;

static std::string tmp(const char* name) {
    std::string p = std::string(FPX_TEST_TMP) + "/" + name;
    std::remove(p.c_str());
    return p;
}

TEST_CASE("Gallery finds the right subject among thousands") {
    Gallery g;
    const int N = 3000;
    for (int i = 0; i < N; ++i) g.insert("s" + std::to_string(i), synthesize_template(uint64_t(i)));
    CHECK(g.size() == size_t(N));
    for (int target : {0, 1234, N - 1}) {
        auto r = g.identify(perturb_template(synthesize_template(uint64_t(target)), uint64_t(target) + 9), {5, 0, 0});
        REQUIRE(!r.candidates.empty());
        CHECK(r.candidates[0].id == "s" + std::to_string(target));
        CHECK(r.candidates[0].score >= kIdentifyThreshold);
        CHECK(r.compared == uint64_t(N));
    }
    // non-enrolled finger: no candidate above the identification threshold
    auto r = g.identify(perturb_template(synthesize_template(999999), 1), {5, 0, 0});
    CHECK((r.candidates.empty() || r.candidates[0].score < kIdentifyThreshold));
}

TEST_CASE("Gallery replace, remove, compact, verify") {
    Gallery g;
    g.insert("a", synthesize_template(1));
    g.insert("b", synthesize_template(2));
    g.insert("a", synthesize_template(3));  // replace
    CHECK(g.size() == 2);
    CHECK(*g.verify("a", synthesize_template(3)) == doctest::Approx(100.f));
    CHECK(*g.verify("a", synthesize_template(1)) < kVerifyThreshold);
    CHECK(g.remove("b"));
    CHECK(!g.remove("b"));
    CHECK(!g.verify("b", synthesize_template(2)).has_value());
    g.compact();
    CHECK(g.size() == 1);
    auto r = g.identify(synthesize_template(3), {3, 0, 0});
    REQUIRE(r.candidates.size() == 1);
    CHECK(r.candidates[0].id == "a");
}

TEST_CASE("Identify is consistent across thread counts") {
    Gallery g;
    for (int i = 0; i < 2000; ++i) g.insert("s" + std::to_string(i), synthesize_template(uint64_t(i)));
    auto probe = perturb_template(synthesize_template(777), 5);
    auto r1 = g.identify(probe, {5, 0, 1});
    auto r4 = g.identify(probe, {5, 0, 4});
    REQUIRE(r1.candidates.size() == r4.candidates.size());
    for (size_t i = 0; i < r1.candidates.size(); ++i) CHECK(r1.candidates[i].score == r4.candidates[i].score);
}

TEST_CASE("TemplateStore replays puts/erases and survives a torn tail") {
    const std::string path = tmp("store_test.fpxdb");
    {
        TemplateStore s(path);
        s.put("x", {1, 2, 3});
        s.put("y", {4, 5});
        s.put("x", {9});
        s.erase("y");
        s.sync();
    }
    {  // simulate a crash in the middle of a write
        std::ofstream f(path, std::ios::binary | std::ios::app);
        f.write("\x01\x05\x00garbage", 10);
    }
    std::map<std::string, std::vector<uint8_t>> live;
    std::string warn;
    {
        TemplateStore s(path);
        s.replay([&](const std::string& id, const std::vector<uint8_t>& d) { live[id] = d; },
                 [&](const std::string& id) { live.erase(id); }, &warn);
        CHECK(!warn.empty());
        CHECK(live.size() == 1);
        CHECK(live["x"] == std::vector<uint8_t>{9});
        s.put("z", {7});  // must remain readable after the torn tail was dropped
    }
    live.clear();
    TemplateStore s(path);
    s.replay([&](const std::string& id, const std::vector<uint8_t>& d) { live[id] = d; },
             [&](const std::string& id) { live.erase(id); });
    CHECK(live.size() == 2);
    auto before = std::filesystem::file_size(path);
    s.compact();
    CHECK(std::filesystem::file_size(path) < before);
    live.clear();
    s.replay([&](const std::string& id, const std::vector<uint8_t>& d) { live[id] = d; },
             [&](const std::string& id) { live.erase(id); });
    CHECK(live.size() == 2);
    CHECK(live["z"] == std::vector<uint8_t>{7});
}
