#include <doctest/doctest.h>

#include "fpx/gallery.hpp"
#include "fpx/image.hpp"
#include "fpx/store.hpp"
#include "fpx/synthetic.hpp"

using namespace fpx;

namespace {
FingerTemplate finger_of(uint64_t subject, uint8_t pos) { return synthesize_template(subject * 16 + pos); }

void enrol_two(Gallery& g, int n) {
    for (int i = 0; i < n; ++i) {
        const std::string id = "c" + std::to_string(i);
        REQUIRE(g.insert(id, 2, finger_of(uint64_t(i), 2)));
        REQUIRE(g.insert(id, 7, finger_of(uint64_t(i), 7)));
    }
}
}  // namespace

TEST_CASE("Fusion policy decisions") {
    FusionPolicy p;  // finger 12, min 2, single 18, strong 30
    auto cand = [](std::vector<float> scores) {
        SubjectCandidate c;
        for (float s : scores) c.fingers.push_back({0, 0, s});
        return c;
    };
    SubjectCandidate c = cand({15, 14});
    apply_policy(c, 2, p);
    CHECK(c.match);
    CHECK(c.fingers_matched == 2);
    CHECK(c.score == doctest::Approx(29.f));

    c = cand({25});  // second finger presented but scored nothing; not strong enough alone
    apply_policy(c, 2, p);
    CHECK_FALSE(c.match);

    c = cand({35});  // strong single finger accepted
    apply_policy(c, 2, p);
    CHECK(c.match);

    c = cand({17});  // one finger presented -> single-finger 1:N threshold (18)
    apply_policy(c, 1, p);
    CHECK_FALSE(c.match);
    c = cand({18.5f});
    apply_policy(c, 1, p);
    CHECK(c.match);

    p.strong_single_threshold = 0;  // disabled
    c = cand({90});
    apply_policy(c, 2, p);
    CHECK_FALSE(c.match);
}

TEST_CASE("Two-finger identification finds the subject; strangers are rejected") {
    Gallery g;
    const int N = 1500;
    enrol_two(g, N);
    CHECK(g.subjects() == size_t(N));
    CHECK(g.size() == size_t(2 * N));
    for (int target : {0, 777, N - 1}) {
        std::vector<FingerProbe> probes = {{2, perturb_template(finger_of(uint64_t(target), 2), 11)},
                                           {7, perturb_template(finger_of(uint64_t(target), 7), 12)}};
        auto r = g.identify_subjects(probes, {});
        REQUIRE(!r.candidates.empty());
        CHECK(r.candidates[0].id == "c" + std::to_string(target));
        CHECK(r.candidates[0].match);
        CHECK(r.candidates[0].fingers_matched == 2);
        // positions known on both sides -> each probe compared only with its own position
        CHECK(r.compared == uint64_t(2 * N));
        for (auto& f : r.candidates[0].fingers) CHECK(f.probe_finger == f.gallery_finger);
    }
    for (int k = 0; k < 5; ++k) {
        std::vector<FingerProbe> stranger = {{2, perturb_template(finger_of(900000 + k, 2), 1)},
                                             {7, perturb_template(finger_of(900000 + k, 7), 2)}};
        auto r = g.identify_subjects(stranger, {});
        CHECK((r.candidates.empty() || !r.candidates[0].match));
    }
}

TEST_CASE("Unknown probe position is compared with every enrolled finger") {
    Gallery g;
    enrol_two(g, 200);
    std::vector<FingerProbe> probes = {{0, perturb_template(finger_of(42, 7), 3)}};
    auto r = g.identify_subjects(probes, {});
    CHECK(r.compared == 400u);
    REQUIRE(!r.candidates.empty());
    CHECK(r.candidates[0].id == "c42");
    CHECK(r.candidates[0].fingers[0].gallery_finger == 7);
}

TEST_CASE("Wrong finger position does not match (index vs thumb slot)") {
    Gallery g;
    enrol_two(g, 50);
    // right index presented but labelled as right thumb (1): nothing enrolled at 1
    std::vector<FingerProbe> probes = {{1, perturb_template(finger_of(5, 2), 3)}};
    auto r = g.identify_subjects(probes, {});
    CHECK(r.compared == 0u);
    CHECK(r.candidates.empty());
}

TEST_CASE("Finger-level enrolment management") {
    Gallery g;
    g.insert("alice", 2, finger_of(1, 2));
    g.insert("alice", 7, finger_of(1, 7));
    g.insert("bob", finger_of(2, 0));  // legacy single-finger API -> slot 0
    CHECK(g.contains("alice", 2));
    CHECK_FALSE(g.contains("alice", 3));
    CHECK(g.fingers_of("alice").size() == 2);
    CHECK(g.fingers_of("bob")[0].first == 0);
    g.insert("alice", 2, finger_of(3, 2));  // replace one finger
    CHECK(g.size() == 3);
    CHECK(g.remove("alice", 7));
    CHECK_FALSE(g.remove("alice", 7));
    CHECK(g.subjects() == 2);
    CHECK(g.remove_subject("alice") == 1);
    CHECK_FALSE(g.contains("alice"));
    CHECK(g.subjects() == 1);
    g.compact();
    CHECK(g.subjects() == 1);
    CHECK(g.size() == 1);
    auto r = g.identify(finger_of(2, 0), {3, 0, 0});
    REQUIRE(!r.candidates.empty());
    CHECK(r.candidates[0].id == "bob");
    CHECK_THROWS_AS(g.insert("x", 11, finger_of(1, 1)), Error);
}

TEST_CASE("verify_subject fuses both fingers") {
    Gallery g;
    enrol_two(g, 20);
    std::vector<FingerProbe> good = {{2, perturb_template(finger_of(3, 2), 5)}, {7, perturb_template(finger_of(3, 7), 6)}};
    auto v = g.verify_subject("c3", good);
    REQUIRE(v.has_value());
    CHECK(v->match);
    auto w = g.verify_subject("c4", good);
    REQUIRE(w.has_value());
    CHECK_FALSE(w->match);
    CHECK_FALSE(g.verify_subject("nobody", good).has_value());
}

TEST_CASE("Store keys: legacy ids load as finger 0, positions round-trip") {
    CHECK(make_store_key("cust-1", 0) == "cust-1");
    auto [s, f] = split_store_key(make_store_key("cust-1", 7));
    CHECK(s == "cust-1");
    CHECK(f == 7);
    auto legacy = split_store_key("old-id");
    CHECK(legacy.first == "old-id");
    CHECK(legacy.second == 0);
    CHECK_THROWS_AS(split_store_key(std::string("x\x1F") + "12"), Error);
    CHECK(valid_subject_id("customer 42/ä"));
    CHECK_FALSE(valid_subject_id(std::string("a\x1F") + "2"));
    CHECK_FALSE(valid_subject_id(""));
}
