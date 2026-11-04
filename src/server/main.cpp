// SPDX-License-Identifier: MIT
// fpx-server: REST API for enrolment, 1:1 verification and 1:N identification.
//
// Devices (any vendor) only need to send an ANSI-378 / ISO-19794-2 template
// (base64). Images are accepted too for thin clients, but sending templates
// keeps bandwidth tiny (~0.3 KB) and the server device-agnostic.
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>

#include "fpx/fpx.hpp"

using json = nlohmann::json;
using namespace fpx;

namespace {

// --- base64 ---------------------------------------------------------------
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64encode(const std::vector<uint8_t>& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = uint32_t(in[i]) << 16;
        if (i + 1 < in.size()) v |= uint32_t(in[i + 1]) << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::vector<uint8_t> b64decode(const std::string& in) {
    static int8_t T[256];
    static bool init = [] {
        std::fill(std::begin(T), std::end(T), int8_t(-1));
        for (int i = 0; i < 64; ++i) T[uint8_t(kB64[i])] = int8_t(i);
        T[uint8_t('-')] = 62;  // accept base64url too
        T[uint8_t('_')] = 63;
        return true;
    }();
    (void)init;
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (unsigned char c : in) {
        if (c == '=' || std::isspace(c)) continue;
        int8_t v = T[c];
        if (v < 0) throw Error("invalid base64");
        buf = (buf << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// --- config ---------------------------------------------------------------
struct Config {
    std::string db = "gallery.fpxdb";
    std::string host = "0.0.0.0";
    int port = 8080;
    int match_threads = 0;
    // Each keep-alive connection holds an HTTP thread, and searches wait in the
    // gallery's FIFO queue, so keep this well above the expected client count:
    // with 8 threads, 16 clients saw p95 latency 6x p50 (connections starved).
    int http_threads = 64;
    std::string api_key;  // empty = no auth
    float verify_threshold = kVerifyThreshold;
    float identify_threshold = kIdentifyThreshold;  // one finger presented (1:N)
    float finger_threshold = kVerifyThreshold;      // per finger when >= 2 presented
    int min_fingers = 2;
    float strong_single_threshold = 30.0f;
    int default_dpi = 500;
    bool demo = false;     // enables synthetic generation
    std::string web_root;  // serves the built web app when set
};

void usage() {
    std::puts(
        "fpx-server [--db gallery.fpxdb] [--host 0.0.0.0] [--port 8080] [--threads N] [--http-threads 64]\n"
        "           [--api-key KEY]\n"
        "           [--verify-threshold 12]       1:1, one finger\n"
        "           [--identify-threshold 18]     1:N, one finger presented\n"
        "           [--finger-threshold 12]       1:N/1:1, per finger when two or more are presented\n"
        "           [--min-fingers 2]             fingers that must reach --finger-threshold\n"
        "           [--strong-single-threshold 30] one finger alone this strong also matches (0 = off)\n"
        "           [--demo]                      enable synthetic finger generation (never in production)\n"
        "           [--web-root DIR]              serve the built web app from DIR\n"
        "Environment: FPX_API_KEY overrides --api-key.");
}

struct Error400 : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One finger of a request after decoding: position + parsed template + raw record.
struct InFinger {
    uint8_t position = 0;
    FingerTemplate tmpl;
    std::vector<uint8_t> record;
};

json policy_json(const FusionPolicy& p) {
    return {{"finger_threshold", p.finger_threshold},
            {"min_fingers", p.min_fingers},
            {"single_threshold", p.single_threshold},
            {"strong_single_threshold", p.strong_single_threshold}};
}

json candidate_json(const SubjectCandidate& c) {
    json fingers = json::array();
    for (const auto& f : c.fingers)
        fingers.push_back({{"position", f.probe_finger}, {"enrolled_position", f.gallery_finger}, {"score", f.score}});
    return {{"id", c.id},
            {"score", c.score},
            {"match", c.match},
            {"fingers_matched", c.fingers_matched},
            {"fingers", fingers}};
}

class Service {
public:
    explicit Service(const Config& cfg) : cfg_(cfg), store_(cfg.db), gallery_() {
        policy_.finger_threshold = cfg.finger_threshold;
        policy_.min_fingers = cfg.min_fingers;
        policy_.single_threshold = cfg.identify_threshold;
        policy_.strong_single_threshold = cfg.strong_single_threshold;
        verify_policy_ = policy_;
        verify_policy_.single_threshold = cfg.verify_threshold;

        auto t0 = std::chrono::steady_clock::now();
        std::string warn;
        size_t bad = 0;
        store_.replay(
            [&](const std::string& key, const std::vector<uint8_t>& rec) {
                try {
                    auto [subject, finger] = split_store_key(key);
                    gallery_.insert(subject, finger, parse_template(rec));
                } catch (const std::exception&) {
                    ++bad;
                }
            },
            [&](const std::string& key) {
                try {
                    auto [subject, finger] = split_store_key(key);
                    gallery_.remove(subject, finger);
                } catch (const std::exception&) {
                    ++bad;
                }
            },
            &warn);
        gallery_.compact();
        if (!warn.empty()) std::fprintf(stderr, "warning: %s\n", warn.c_str());
        if (bad) std::fprintf(stderr, "warning: %zu undecodable store records skipped\n", bad);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "loaded %zu subjects / %zu finger templates from %s in %.0f ms (%.0f MB)\n",
                     gallery_.subjects(), gallery_.size(), cfg.db.c_str(), ms, gallery_.memory_bytes() / 1048576.0);
    }

    // --- request decoding ------------------------------------------------
    static uint8_t parse_position(const json& j, uint8_t fallback) {
        const char* key = j.contains("position") ? "position" : j.contains("finger") ? "finger" : nullptr;
        if (!key) return fallback;
        if (!j[key].is_number_integer()) throw Error400("finger position must be an integer 0..10");
        int p = j[key].get<int>();
        if (p < 0 || p > kMaxFingerPosition) throw Error400("finger position must be 0..10");
        return uint8_t(p);
    }

    // One finger from {"template": b64} or {"image": b64, "dpi": N}; position
    // from "position"/"finger", else `default_position`, else the template header.
    InFinger finger_from(const json& j, int default_position = -1, const char* tkey = "template",
                         const char* ikey = "image") {
        if (!j.is_object()) throw Error400("each finger must be a JSON object");
        InFinger f;
        if (j.contains(tkey)) {
            f.record = b64decode(j.at(tkey).get<std::string>());
            try {
                f.tmpl = parse_template(f.record);
            } catch (const Error& e) {
                throw Error400(std::string("bad template: ") + e.what());
            }
        } else if (j.contains(ikey)) {
            auto bytes = b64decode(j.at(ikey).get<std::string>());
            try {
                auto img = decode_image(bytes, j.value("dpi", cfg_.default_dpi));
                if (j.contains("dpi")) img.dpi = j["dpi"].get<int>();
                ExtractOptions o;
                o.format = parse_template_format(j.value("format", std::string("iso")));
                o.finger_position = parse_position(j, default_position > 0 ? uint8_t(default_position) : 0);
                auto r = extract(img, o);
                f.tmpl = r.parsed;
                f.record = r.record;
            } catch (const Error400&) {
                throw;
            } catch (const Error& e) {
                throw Error400(std::string("bad image: ") + e.what());
            }
        } else {
            throw Error400(std::string("finger needs '") + tkey + "' (base64 ANSI/ISO template) or '" + ikey +
                           "' (base64 BMP/PGM/TIFF/WSQ)");
        }
        const uint8_t header_pos = f.tmpl.finger_position <= kMaxFingerPosition ? f.tmpl.finger_position : 0;
        f.position = parse_position(j, default_position >= 0 ? uint8_t(default_position) : header_pos);
        return f;
    }

    // {"fingers": [...]} or a single legacy {"template"|"image", "position"?}.
    std::vector<InFinger> fingers_from(const json& j) {
        std::vector<InFinger> out;
        if (j.contains("fingers")) {
            const json& arr = j["fingers"];
            if (!arr.is_array() || arr.empty()) throw Error400("'fingers' must be a non-empty array");
            if (arr.size() > size_t(kMaxFingerPosition)) throw Error400("at most 10 fingers per request");
            for (const auto& f : arr) out.push_back(finger_from(f));
        } else {
            out.push_back(finger_from(j));
        }
        uint32_t seen = 0;
        for (const auto& f : out) {
            if ((seen >> f.position) & 1u)
                throw Error400("finger position " + std::to_string(f.position) + " given twice" +
                               (f.position == 0 ? " (set \"position\" 1..10 when sending several fingers)" : ""));
            seen |= 1u << f.position;
        }
        return out;
    }

    static std::vector<FingerProbe> probes_of(const std::vector<InFinger>& in) {
        std::vector<FingerProbe> p;
        for (const auto& f : in) p.push_back({f.position, f.tmpl});
        return p;
    }

    void check_enrolable(const std::vector<InFinger>& fingers) {
        for (const auto& f : fingers)
            if (f.tmpl.minutiae.size() < 6)
                throw Error400("finger " + std::to_string(f.position) + ": too few minutiae (" +
                               std::to_string(f.tmpl.minutiae.size()) + "), please re-scan");
    }

    static void check_id(const std::string& id) {
        if (!valid_subject_id(id)) throw Error400("id must be 1..256 characters without control characters");
    }

    // --- endpoints ---------------------------------------------------------
    json stats() const {
        return {{"version", FPX_VERSION_STRING},
                {"subjects", gallery_.subjects()},
                {"templates", gallery_.size()},
                {"memory_mb", gallery_.memory_bytes() / 1048576.0},
                {"verify_threshold", cfg_.verify_threshold},
                {"identify_threshold", cfg_.identify_threshold},
                {"policy", policy_json(policy_)},
                {"match_threads", cfg_.match_threads > 0 ? cfg_.match_threads : int(std::thread::hardware_concurrency())},
                {"matcher", to_string(gallery_.matcher().config())}};
    }

    json extract_ep(const json& j) {
        if (!j.contains("image")) throw Error400("'image' (base64) is required");
        auto bytes = b64decode(j["image"].get<std::string>());
        GrayImage img;
        try {
            img = decode_image(bytes, j.value("dpi", cfg_.default_dpi));
        } catch (const Error& e) {
            throw Error400(std::string("bad image: ") + e.what());
        }
        if (j.contains("dpi")) img.dpi = j["dpi"].get<int>();
        ExtractOptions o;
        o.format = parse_template_format(j.value("format", std::string("iso")));
        o.finger_position = parse_position(j, 0);
        ExtractResult r;
        try {
            r = extract(img, o);
        } catch (const Error& e) {
            throw Error400(e.what());
        }
        json out = {{"template", b64encode(r.record)},
                    {"format", to_string(o.format)},
                    {"position", o.finger_position},
                    {"minutiae", r.parsed.minutiae.size()},
                    {"width", img.width},
                    {"height", img.height},
                    {"dpi", img.dpi}};
        if (j.value("wsq", false)) out["wsq"] = b64encode(wsq_encode(img, j.value("wsq_bitrate", 0.75f)));
        return out;
    }

    json subject_json(const std::string& id) const {
        json fingers = json::array();
        for (auto& [pos, nm] : gallery_.fingers_of(id)) fingers.push_back({{"position", pos}, {"minutiae", nm}});
        return {{"id", id}, {"fingers", fingers}};
    }

    std::optional<json> get_subject(const std::string& id) const {
        if (!gallery_.contains(id)) return std::nullopt;
        return subject_json(id);
    }

    // PUT /v1/subjects/{id}: the subject ends up with exactly these fingers.
    json put_subject(const std::string& id, const json& j, bool& created) {
        check_id(id);
        auto fingers = fingers_from(j);  // decode/extract outside the lock
        check_enrolable(fingers);
        std::lock_guard<std::mutex> lk(write_mu_);
        created = !gallery_.contains(id);
        uint32_t keep = 0;
        for (const auto& f : fingers) keep |= 1u << f.position;
        for (auto& [pos, nm] : gallery_.fingers_of(id)) {
            if ((keep >> pos) & 1u) continue;
            store_.erase(make_store_key(id, pos));
            gallery_.remove(id, pos);
        }
        for (const auto& f : fingers) {
            store_.put(make_store_key(id, f.position), f.record);  // durable first, then visible
            gallery_.insert(id, f.position, f.tmpl);
        }
        return subject_json(id);
    }

    // PUT /v1/subjects/{id}/fingers/{pos}: add or replace one finger.
    json put_finger(const std::string& id, int pos, const json& j, bool& created) {
        check_id(id);
        if (pos < 0 || pos > kMaxFingerPosition) throw Error400("finger position must be 0..10");
        InFinger f = finger_from(j, pos);
        f.position = uint8_t(pos);  // the URL is authoritative
        check_enrolable({f});
        std::lock_guard<std::mutex> lk(write_mu_);
        created = !gallery_.contains(id, f.position);
        store_.put(make_store_key(id, f.position), f.record);
        gallery_.insert(id, f.position, f.tmpl);
        return subject_json(id);
    }

    bool delete_subject(const std::string& id) {
        std::lock_guard<std::mutex> lk(write_mu_);
        auto fingers = gallery_.fingers_of(id);
        if (fingers.empty()) return false;
        for (auto& [pos, nm] : fingers) store_.erase(make_store_key(id, pos));
        gallery_.remove_subject(id);
        return true;
    }

    bool delete_finger(const std::string& id, int pos) {
        if (pos < 0 || pos > kMaxFingerPosition) throw Error400("finger position must be 0..10");
        std::lock_guard<std::mutex> lk(write_mu_);
        if (!gallery_.contains(id, uint8_t(pos))) return false;
        store_.erase(make_store_key(id, uint8_t(pos)));
        gallery_.remove(id, uint8_t(pos));
        return true;
    }

    json identify(const json& j) {
        auto fingers = fingers_from(j);
        SubjectIdentifyOptions o;
        o.top_k = std::clamp(j.value("top_k", 5), 1, 100);
        o.threads = cfg_.match_threads;
        o.policy = policy_;
        if (fingers.size() == 1 && j.contains("threshold")) o.policy.single_threshold = j["threshold"].get<float>();
        auto r = gallery_.identify_subjects(probes_of(fingers), o);
        json cands = json::array();
        for (auto& c : r.candidates) cands.push_back(candidate_json(c));
        const bool hit = !r.candidates.empty() && r.candidates[0].match;
        return {{"match", hit},
                {"id", hit ? json(r.candidates[0].id) : json(nullptr)},
                {"fingers_presented", fingers.size()},
                {"threshold", fingers.size() == 1 ? o.policy.single_threshold : o.policy.finger_threshold},
                {"policy", policy_json(o.policy)},
                {"candidates", cands},
                {"compared", r.compared},
                {"elapsed_ms", r.elapsed_ms}};
    }

    std::optional<json> verify(const json& j) {
        if (!j.contains("id")) throw Error400("'id' is required");
        const std::string id = j["id"].get<std::string>();
        auto fingers = fingers_from(j);
        FusionPolicy p = verify_policy_;
        if (fingers.size() == 1 && j.contains("threshold")) p.single_threshold = j["threshold"].get<float>();
        auto c = gallery_.verify_subject(id, probes_of(fingers), p);
        if (!c) return std::nullopt;
        json out = candidate_json(*c);
        out["fingers_presented"] = fingers.size();
        out["threshold"] = fingers.size() == 1 ? p.single_threshold : p.finger_threshold;
        out["policy"] = policy_json(p);
        return out;
    }

    json match(const json& j) {
        auto a = finger_from(j, -1, "a", "a_image").tmpl;
        auto b = finger_from(j, -1, "b", "b_image").tmpl;
        float s = gallery_.matcher().match(a, b);
        const float thr = j.value("threshold", cfg_.verify_threshold);
        return {{"score", s}, {"threshold", thr}, {"match", s >= thr}};
    }

    // --- demo and test bench endpoints --------------------------------------
    // Any upload -> template, following BMP -> WSQ -> MINEX for images.
    struct Inspected {
        std::string kind, source_format;
        GrayImage image;
        std::vector<uint8_t> wsq;
        FingerTemplate tmpl;
        json steps = json::array();
    };

    Inspected inspect_bytes(const std::vector<uint8_t>& bytes, int dpi, float bitrate) const {
        using clock = std::chrono::steady_clock;
        auto ms_since = [](clock::time_point t) {
            return std::chrono::duration<double, std::milli>(clock::now() - t).count();
        };
        Inspected r;
        if (bytes.empty()) throw Error400("file is empty");
        if (looks_like_template(bytes)) {
            r.kind = "template";
            try {
                r.tmpl = parse_template(bytes);
            } catch (const Error& e) {
                throw Error400(std::string("bad template: ") + e.what());
            }
            r.source_format = to_string(r.tmpl.format);
            return r;
        }
        const ImageFormat fmt = detect_image_format(bytes);
        if (fmt == ImageFormat::Unknown) throw Error400("unrecognised file: expected BMP, PGM, TIFF, WSQ, MINEX or ISO");
        try {
            r.kind = "image";
            r.source_format = to_string(fmt);
            auto t = clock::now();
            r.image = decode_image(bytes, dpi);
            r.steps.push_back({{"name", "decode"}, {"ms", ms_since(t)}, {"bytes", bytes.size()}});
            t = clock::now();
            r.wsq = fmt == ImageFormat::Wsq ? bytes : wsq_encode(r.image, bitrate);
            GrayImage decoded = wsq_decode(r.wsq, r.image.dpi);
            decoded.dpi = r.image.dpi;
            r.steps.push_back({{"name", "wsq"}, {"ms", ms_since(t)}, {"bytes", r.wsq.size()}});
            t = clock::now();
            ExtractOptions o;
            o.format = TemplateFormat::Ansi378_2004;
            auto ex = extract(decoded, o);
            r.tmpl = ex.parsed;
            r.steps.push_back({{"name", "minex"}, {"ms", ms_since(t)}, {"bytes", ex.record.size()}});
        } catch (const Error400&) {
            throw;
        } catch (const Error& e) {
            throw Error400(std::string("bad image: ") + e.what());
        }
        return r;
    }

    static std::vector<uint8_t> file_field(const json& j, const char* key) {
        if (!j.contains(key) || !j[key].is_string()) throw Error400(std::string("'") + key + "' (base64 file) is required");
        return b64decode(j[key].get<std::string>());
    }

    json inspect(const json& j) const {
        Inspected r = inspect_bytes(file_field(j, "file"), j.value("dpi", cfg_.default_dpi), j.value("wsq_bitrate", 0.75f));
        json minutiae = json::array();
        for (const Minutia& m : r.tmpl.minutiae)
            minutiae.push_back({{"x", m.x}, {"y", m.y}, {"angle", m.angle}, {"type", int(m.type)}, {"quality", m.quality}});
        json out = {{"kind", r.kind},
                    {"source_format", r.source_format},
                    {"width", r.tmpl.width},
                    {"height", r.tmpl.height},
                    {"minutiae", minutiae},
                    {"steps", r.steps},
                    {"template_ansi", b64encode(serialize_template(r.tmpl, TemplateFormat::Ansi378_2004))},
                    {"template_iso", b64encode(serialize_template(r.tmpl, TemplateFormat::Iso19794_2_2005))}};
        if (r.kind == "image") {
            out["preview_bmp"] = b64encode(encode_bmp(r.image));
            out["wsq"] = b64encode(r.wsq);
            out["dpi"] = r.image.dpi;
        }
        return out;
    }

    json compare(const json& j) const {
        const int dpi = j.value("dpi", cfg_.default_dpi);
        FingerTemplate a = inspect_bytes(file_field(j, "a"), dpi, 0.75f).tmpl;
        FingerTemplate b = inspect_bytes(file_field(j, "b"), dpi, 0.75f).tmpl;
        const Matcher& matcher = gallery_.matcher();
        const float score = matcher.match(a, b);
        Probe probe(a, matcher.config());
        CompiledTemplate cb = compile(b, matcher.config());
        MatchDetails d;
        matcher.match(probe, cb.view(), &d, true);
        json pairs = json::array();
        for (auto& [pi, ci] : d.pairs) {
            const CMinutia& pm = probe.tmpl().minutiae[size_t(pi)];
            const CMinutia& cm = cb.minutiae[size_t(ci)];
            pairs.push_back({{"a", {{"x", pm.x}, {"y", pm.y}}}, {"b", {{"x", cm.x}, {"y", cm.y}}}});
        }
        const float thr = j.value("threshold", cfg_.verify_threshold);
        return {{"score", score},
                {"threshold", thr},
                {"match", score >= thr},
                {"rotation_deg", d.rotation_deg},
                {"pairs", pairs}};
    }

    json crossmatch(const json& j) const {
        if (!j.contains("files") || !j["files"].is_array() || j["files"].size() < 2)
            throw Error400("'files' must list at least 2 files");
        if (j["files"].size() > 100) throw Error400("at most 100 files per cross-match");
        const int dpi = j.value("dpi", cfg_.default_dpi);
        std::vector<std::string> names;
        std::vector<FingerTemplate> templates;
        for (const auto& f : j["files"]) {
            const std::string name = f.value("name", "file " + std::to_string(names.size() + 1));
            try {
                templates.push_back(inspect_bytes(file_field(f, "file"), dpi, 0.75f).tmpl);
            } catch (const std::exception& e) {
                throw Error400(name + ": " + e.what());
            }
            names.push_back(name);
        }
        auto t0 = std::chrono::steady_clock::now();
        const Matcher& matcher = gallery_.matcher();
        std::vector<std::vector<float>> scores(names.size(), std::vector<float>(names.size(), 0.f));
        for (size_t a = 0; a < names.size(); ++a)
            for (size_t b = a; b < names.size(); ++b) scores[a][b] = scores[b][a] = matcher.match(templates[a], templates[b]);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return {{"names", names}, {"scores", scores}, {"threshold", cfg_.verify_threshold}, {"elapsed_ms", ms}};
    }

    static json synthetic(const json& j) {
        if (!j.contains("finger_id") || !j["finger_id"].is_number_unsigned())
            throw Error400("'finger_id' must be a non-negative integer");
        const uint32_t finger = j["finger_id"].get<uint32_t>();
        const uint32_t impression = j.value("impression", 0u);
        return {{"file", b64encode(encode_bmp(synthesize_image(finger, impression)))}};
    }

    // Rewrite the store with live records only and repack the gallery. Done
    // inside the server so the open store file is swapped safely.
    json compact() {
        std::lock_guard<std::mutex> lk(write_mu_);
        const std::string path = cfg_.db;
        auto before = std::filesystem::file_size(path);
        auto t0 = std::chrono::steady_clock::now();
        store_.compact();
        gallery_.compact();
        auto after = std::filesystem::file_size(path);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return {{"bytes_before", before}, {"bytes_after", after}, {"elapsed_ms", ms}};
    }

    void flush() { store_.sync(); }
    const Config& cfg() const { return cfg_; }

private:
    Config cfg_;
    TemplateStore store_;
    Gallery gallery_;
    FusionPolicy policy_, verify_policy_;
    std::mutex write_mu_;  // serialises enrol/delete so store and gallery stay in step
};

httplib::Server* g_server = nullptr;
void on_signal(int) {
    if (g_server) g_server->stop();
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--db") cfg.db = next();
        else if (a == "--host") cfg.host = next();
        else if (a == "--port") cfg.port = std::stoi(next());
        else if (a == "--threads") cfg.match_threads = std::stoi(next());
        else if (a == "--http-threads") cfg.http_threads = std::stoi(next());
        else if (a == "--api-key") cfg.api_key = next();
        else if (a == "--verify-threshold") cfg.verify_threshold = std::stof(next());
        else if (a == "--identify-threshold") cfg.identify_threshold = std::stof(next());
        else if (a == "--finger-threshold") cfg.finger_threshold = std::stof(next());
        else if (a == "--min-fingers") cfg.min_fingers = std::stoi(next());
        else if (a == "--strong-single-threshold") cfg.strong_single_threshold = std::stof(next());
        else if (a == "--demo") cfg.demo = true;
        else if (a == "--web-root") cfg.web_root = next();
        else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    if (const char* k = std::getenv("FPX_API_KEY")) cfg.api_key = k;

    std::unique_ptr<Service> svc;
    try {
        svc = std::make_unique<Service>(cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "startup failed: %s\n", e.what());
        return 1;
    }

    httplib::Server srv;
    g_server = &srv;
    const int http_threads = cfg.http_threads;
    srv.new_task_queue = [http_threads] { return new httplib::ThreadPool(size_t(http_threads)); };
    srv.set_payload_max_length(64 * 1024 * 1024);  // cross-match lists can hold 100 images
    if (!cfg.web_root.empty() && !srv.set_mount_point("/", cfg.web_root)) {
        std::fprintf(stderr, "web root not found: %s\n", cfg.web_root.c_str());
        return 1;
    }

    // Auth + JSON error handling wrapper.
    auto handler = [&](auto fn) {
        return [&, fn](const httplib::Request& req, httplib::Response& res) {
            if (!svc->cfg().api_key.empty()) {
                const std::string& key = req.get_header_value("X-API-Key");
                const std::string& want = svc->cfg().api_key;
                unsigned diff = unsigned(key.size() ^ want.size());
                for (size_t i = 0; i < want.size(); ++i) diff |= unsigned(i < key.size() ? key[i] ^ want[i] : 1);
                if (diff) {
                    res.status = 401;
                    res.set_content(R"({"error":"missing or invalid X-API-Key"})", "application/json");
                    return;
                }
            }
            try {
                json body = req.body.empty() ? json::object() : json::parse(req.body);
                json out = fn(req, body, res);
                if (res.status == -1 || res.status == 0) res.status = 200;
                res.set_content(out.dump(), "application/json");
            } catch (const json::exception& e) {
                res.status = 400;
                res.set_content(json{{"error", std::string("invalid JSON: ") + e.what()}}.dump(), "application/json");
            } catch (const Error400& e) {
                res.status = 400;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            } catch (const Error& e) {
                res.status = 400;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        };
    };

    srv.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });
    srv.Get("/v1/stats", handler([&](const httplib::Request&, const json&, httplib::Response&) { return svc->stats(); }));
    srv.Post("/v1/extract",
             handler([&](const httplib::Request&, const json& j, httplib::Response&) { return svc->extract_ep(j); }));

    // Subjects (customers) and their fingers.
    auto put_subject = handler([&](const httplib::Request& req, const json& j, httplib::Response& res) {
        bool created = false;
        json out = svc->put_subject(req.matches[1], j, created);
        res.status = created ? 201 : 200;
        return out;
    });
    srv.Put(R"(/v1/subjects/([^/]+))", put_subject);
    srv.Post(R"(/v1/subjects/([^/]+))", put_subject);  // v2.0 compatibility
    srv.Get(R"(/v1/subjects/([^/]+))", handler([&](const httplib::Request& req, const json&, httplib::Response& res) {
                auto s = svc->get_subject(req.matches[1]);
                if (!s) {
                    res.status = 404;
                    return json{{"error", "unknown id"}};
                }
                return *s;
            }));
    srv.Delete(R"(/v1/subjects/([^/]+))", handler([&](const httplib::Request& req, const json&, httplib::Response& res) {
                   if (!svc->delete_subject(req.matches[1])) {
                       res.status = 404;
                       return json{{"error", "unknown id"}};
                   }
                   return json{{"id", std::string(req.matches[1])}, {"deleted", true}};
               }));
    auto put_finger = handler([&](const httplib::Request& req, const json& j, httplib::Response& res) {
        bool created = false;
        json out = svc->put_finger(req.matches[1], std::stoi(req.matches[2]), j, created);
        res.status = created ? 201 : 200;
        return out;
    });
    srv.Put(R"(/v1/subjects/([^/]+)/fingers/(\d{1,2}))", put_finger);
    srv.Post(R"(/v1/subjects/([^/]+)/fingers/(\d{1,2}))", put_finger);
    srv.Delete(R"(/v1/subjects/([^/]+)/fingers/(\d{1,2}))",
               handler([&](const httplib::Request& req, const json&, httplib::Response& res) {
                   if (!svc->delete_finger(req.matches[1], std::stoi(req.matches[2]))) {
                       res.status = 404;
                       return json{{"error", "unknown id or finger"}};
                   }
                   return json{{"id", std::string(req.matches[1])},
                               {"position", std::stoi(req.matches[2])},
                               {"deleted", true}};
               }));

    srv.Post("/v1/identify",
             handler([&](const httplib::Request&, const json& j, httplib::Response&) { return svc->identify(j); }));
    srv.Post("/v1/verify", handler([&](const httplib::Request&, const json& j, httplib::Response& res) {
                 auto out = svc->verify(j);
                 if (!out) {
                     res.status = 404;
                     return json{{"error", "unknown id"}};
                 }
                 return *out;
             }));
    srv.Post("/v1/admin/compact",
             handler([&](const httplib::Request&, const json&, httplib::Response&) { return svc->compact(); }));
    srv.Post("/v1/match", handler([&](const httplib::Request&, const json& j, httplib::Response&) { return svc->match(j); }));
    srv.Post("/v1/inspect", handler([&](const httplib::Request&, const json& j, httplib::Response&) { return svc->inspect(j); }));
    srv.Post("/v1/compare", handler([&](const httplib::Request&, const json& j, httplib::Response&) { return svc->compare(j); }));
    srv.Post("/v1/crossmatch",
             handler([&](const httplib::Request&, const json& j, httplib::Response&) { return svc->crossmatch(j); }));
    if (cfg.demo) {
        srv.Post("/v1/demo/synthetic",
                 handler([&](const httplib::Request&, const json& j, httplib::Response&) { return Service::synthetic(j); }));
        std::fprintf(stderr, "DEMO MODE: synthetic generation enabled, not for production\n");
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::fprintf(stderr, "fpx-server %s listening on http://%s:%d (auth %s)\n", FPX_VERSION_STRING, cfg.host.c_str(),
                 cfg.port, cfg.api_key.empty() ? "disabled" : "X-API-Key");
    bool ok = srv.listen(cfg.host, cfg.port);
    svc->flush();
    std::fprintf(stderr, "server stopped\n");
    return ok ? 0 : 1;
}
