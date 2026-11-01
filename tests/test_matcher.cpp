#include <doctest/doctest.h>

#include "fpx/matcher.hpp"
#include "fpx/synthetic.hpp"

using namespace fpx;

TEST_CASE("Self match scores 100") {
    Matcher m;
    FingerTemplate t = synthesize_template(11);
    CHECK(m.match(t, t) == doctest::Approx(100.f));
}

TEST_CASE("Score is symmetric") {
    Matcher m;
    FingerTemplate a = synthesize_template(5), b = perturb_template(a, 3);
    CHECK(m.match(a, b) == doctest::Approx(m.match(b, a)));
}

TEST_CASE("Genuine (rotated, shifted, noisy) pairs score above threshold, impostors below") {
    Matcher m;
    int genuine_ok = 0, impostor_ok = 0;
    const int N = 40;
    for (int i = 0; i < N; ++i) {
        FingerTemplate t = synthesize_template(1000 + i);
        FingerTemplate g = perturb_template(t, 77 + i);
        FingerTemplate imp = perturb_template(synthesize_template(5000 + i), 99 + i);
        genuine_ok += m.match(t, g) >= kVerifyThreshold;
        impostor_ok += m.match(t, imp) < kVerifyThreshold;
    }
    CHECK(genuine_ok >= N - 1);
    CHECK(impostor_ok == N);
}

TEST_CASE("Pure rotation and translation are recovered") {
    PerturbOptions p;
    p.position_jitter = 0;
    p.angle_jitter = 0;
    p.drop_rate = 0;
    p.spurious_rate = 0;
    p.max_rotation_deg = 30;
    p.max_shift = 30;
    Matcher m;
    FingerTemplate t = synthesize_template(21);
    FingerTemplate r = perturb_template(t, 1, p);
    MatchDetails d;
    float s = m.match(t, r, &d);
    CHECK(s > 60.f);
    CHECK(d.matched >= int(r.minutiae.size() * 0.8));
}

TEST_CASE("Resolution is normalised (1000 dpi template vs 500 dpi template)") {
    FingerTemplate t = synthesize_template(31);
    FingerTemplate hi = t;
    hi.xres = hi.yres = 394;  // 1000 dpi
    hi.width = uint16_t(t.width * 2);
    hi.height = uint16_t(t.height * 2);
    for (auto& mi : hi.minutiae) {
        mi.x = uint16_t(mi.x * 2);
        mi.y = uint16_t(mi.y * 2);
    }
    CHECK(Matcher().match(t, hi) > 90.f);
}

TEST_CASE("Degenerate inputs give 0, not crashes") {
    Matcher m;
    FingerTemplate empty, t = synthesize_template(3);
    CHECK(m.match(empty, t) == 0.f);
    FingerTemplate two = t;
    two.minutiae.resize(2);
    CHECK(m.match(two, t) == 0.f);
}

TEST_CASE("Matcher config parsing") {
    MatcherConfig c = parse_matcher_config("neighbors=5,edge_dist_tol_rel=0.1");
    CHECK(c.neighbors == 5);
    CHECK(c.edge_dist_tol_rel == doctest::Approx(0.1f));
    CHECK_THROWS(parse_matcher_config("nope=1"));
    CHECK(to_string(c).find("neighbors=5") != std::string::npos);
}
