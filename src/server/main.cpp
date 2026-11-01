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
#include <cstdio>
#include <nlohmann/json.hpp>
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
    int http_threads = 8;
    std::string api_key;  // empty = no auth
    float verify_threshold = kVerifyThreshold;
    float identify_threshold = kIdentifyThreshold;
    int default_dpi = 500;
};

void usage() {
    std::puts(
        "fpx-server [--db gallery.fpxdb] [--host 0.0.0.0] [--port 8080] [--threads N]\n"
        "           [--http-threads 8] [--api-key KEY] [--verify-threshold 12] [--identify-threshold 18]\n"
        "Environment: FPX_API_KEY overrides --api-key.");
}

struct Error400 : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Service {
public:
    explicit Service(const Config& cfg) : cfg_(cfg), store_(cfg.db), gallery_() {
        auto t0 = std::chrono::steady_clock::now();
        std::string warn;
        size_t bad = 0;
        store_.replay(
            [&](const std::string& id, const std::vector<uint8_t>& rec) {
                try {
                    gallery_.insert(id, parse_template(rec));
                } catch (const std::exception&) {
                    ++bad;
                }
            },
            [&](const std::string& id) { gallery_.remove(id); }, &warn);
        gallery_.compact();
        if (!warn.empty()) std::fprintf(stderr, "warning: %s\n", warn.c_str());
        if (bad) std::fprintf(stderr, "warning: %zu undecodable templates skipped\n", bad);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "loaded %zu templates from %s in %.0f ms (%.0f MB)\n", gallery_.size(), cfg.db.c_str(), ms,
                     gallery_.memory_bytes() / 1048576.0);
    }

    // Template from {"template": b64} or {"image": b64, "dpi": N}.
    std::pair<FingerTemplate, std::vector<uint8_t>> template_from(const json& j, const char* tkey = "template",
                                                                  const char* ikey = "image") {
        if (j.contains(tkey)) {
            auto bytes = b64decode(j.at(tkey).get<std::string>());
            try {
                return {parse_template(bytes), bytes};
            } catch (const Error& e) {
                throw Error400(std::string("bad template: ") + e.what());
            }
        }
        if (j.contains(ikey)) {
            auto bytes = b64decode(j.at(ikey).get<std::string>());
            try {
                auto img = decode_image(bytes, j.value("dpi", cfg_.default_dpi));
                if (j.contains("dpi")) img.dpi = j["dpi"].get<int>();
                ExtractOptions o;
                o.format = parse_template_format(j.value("format", std::string("iso")));
                auto r = extract(img, o);
                return {r.parsed, r.record};
            } catch (const Error& e) {
                throw Error400(std::string("bad image: ") + e.what());
            }
        }
        throw Error400(std::string("request needs '") + tkey + "' (base64 ANSI/ISO template) or '" + ikey +
                       "' (base64 BMP/PGM/TIFF/WSQ)");
    }

    json stats() const {
        return {{"version", FPX_VERSION_STRING},
                {"templates", gallery_.size()},
                {"memory_mb", gallery_.memory_bytes() / 1048576.0},
                {"verify_threshold", cfg_.verify_threshold},
                {"identify_threshold", cfg_.identify_threshold},
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
        o.finger_position = uint8_t(j.value("finger_position", 0));
        ExtractResult r;
        try {
            r = extract(img, o);
        } catch (const Error& e) {
            throw Error400(e.what());
        }
        json out = {{"template", b64encode(r.record)},
                    {"format", to_string(o.format)},
                    {"minutiae", r.parsed.minutiae.size()},
                    {"width", img.width},
                    {"height", img.height},
                    {"dpi", img.dpi}};
        if (j.value("wsq", false)) out["wsq"] = b64encode(wsq_encode(img, j.value("wsq_bitrate", 0.75f)));
        return out;
    }

    json enroll(const std::string& id, const json& j) {
        if (id.empty() || id.size() > 256) throw Error400("id must be 1..256 characters");
        auto [t, rec] = template_from(j);
        if (t.minutiae.size() < 6) throw Error400("template has too few minutiae (" + std::to_string(t.minutiae.size()) + ")");
        bool replaced = gallery_.contains(id);
        store_.put(id, rec);  // durable first, then visible
        gallery_.insert(id, t);
        return {{"id", id}, {"minutiae", t.minutiae.size()}, {"replaced", replaced}};
    }

    bool remove(const std::string& id) {
        if (!gallery_.contains(id)) return false;
        store_.erase(id);
        gallery_.remove(id);
        return true;
    }

    json verify(const json& j) {
        if (!j.contains("id")) throw Error400("'id' is required");
        const std::string id = j["id"].get<std::string>();
        auto [t, rec] = template_from(j);
        auto s = gallery_.verify(id, t);
        if (!s) return nullptr;
        const float thr = j.value("threshold", cfg_.verify_threshold);
        return {{"id", id}, {"score", *s}, {"threshold", thr}, {"match", *s >= thr}};
    }

    json identify(const json& j) {
        auto [t, rec] = template_from(j);
        IdentifyOptions o;
        o.top_k = std::clamp(j.value("top_k", 5), 1, 100);
        o.threads = cfg_.match_threads;
        const float thr = j.value("threshold", cfg_.identify_threshold);
        auto r = gallery_.identify(t, o);
        json cands = json::array();
        for (auto& c : r.candidates) cands.push_back({{"id", c.id}, {"score", c.score}, {"match", c.score >= thr}});
        const bool hit = !r.candidates.empty() && r.candidates[0].score >= thr;
        return {{"match", hit},
                {"id", hit ? json(r.candidates[0].id) : json(nullptr)},
                {"threshold", thr},
                {"candidates", cands},
                {"compared", r.compared},
                {"elapsed_ms", r.elapsed_ms}};
    }

    json match(const json& j) {
        auto a = template_from(j, "a", "a_image").first;
        auto b = template_from(j, "b", "b_image").first;
        float s = gallery_.matcher().match(a, b);
        const float thr = j.value("threshold", cfg_.verify_threshold);
        return {{"score", s}, {"threshold", thr}, {"match", s >= thr}};
    }

    void flush() { store_.sync(); }
    const Config& cfg() const { return cfg_; }

private:
    Config cfg_;
    TemplateStore store_;
    Gallery gallery_;
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
    srv.set_payload_max_length(20 * 1024 * 1024);

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
    srv.Put(R"(/v1/subjects/([^/]+))", handler([&](const httplib::Request& req, const json& j, httplib::Response& res) {
                json out = svc->enroll(req.matches[1], j);
                res.status = out["replaced"].get<bool>() ? 200 : 201;
                return out;
            }));
    srv.Post(R"(/v1/subjects/([^/]+))", handler([&](const httplib::Request& req, const json& j, httplib::Response& res) {
                 json out = svc->enroll(req.matches[1], j);
                 res.status = out["replaced"].get<bool>() ? 200 : 201;
                 return out;
             }));
    srv.Delete(R"(/v1/subjects/([^/]+))", handler([&](const httplib::Request& req, const json&, httplib::Response& res) {
                   if (!svc->remove(req.matches[1])) {
                       res.status = 404;
                       return json{{"error", "unknown id"}};
                   }
                   return json{{"id", std::string(req.matches[1])}, {"deleted", true}};
               }));
    srv.Post("/v1/verify", handler([&](const httplib::Request&, const json& j, httplib::Response& res) {
                 json out = svc->verify(j);
                 if (out.is_null()) {
                     res.status = 404;
                     return json{{"error", "unknown id"}};
                 }
                 return out;
             }));
    srv.Post("/v1/identify",
             handler([&](const httplib::Request&, const json& j, httplib::Response&) { return svc->identify(j); }));
    srv.Post("/v1/match", handler([&](const httplib::Request&, const json& j, httplib::Response&) { return svc->match(j); }));

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::fprintf(stderr, "fpx-server %s listening on http://%s:%d (auth %s)\n", FPX_VERSION_STRING, cfg.host.c_str(),
                 cfg.port, cfg.api_key.empty() ? "disabled" : "X-API-Key");
    bool ok = srv.listen(cfg.host, cfg.port);
    svc->flush();
    std::fprintf(stderr, "server stopped\n");
    return ok ? 0 : 1;
}
