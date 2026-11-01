// SPDX-License-Identifier: MIT
// `fpx` - command line front-end for fp-extractor.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fpx/fpx.hpp"

namespace fs = std::filesystem;
using namespace fpx;

namespace {

const char* kUsage = R"(fpx - fingerprint toolkit (bitmap -> WSQ / ANSI-378 "MINEX" / ISO-19794-2, matching, 1:N search)

USAGE: fpx <command> [options]

Conversion / extraction
  info <file>                         Describe an image (BMP/PGM/TIFF/WSQ) or a template (ANSI/ISO)
  convert <in> <out>                  Convert images (.bmp .pgm .wsq), e.g. scanner.bmp -> scanner.wsq
        [--dpi N] [--bitrate 0.75]
  extract <image> -o <out.tmpl>       Extract minutiae with FingerJet
        [--format iso|ansi] [--dpi N] [--finger 0-10] [--wsq out.wsq]
  totemplate <tmpl> <out> --format iso|ansi   Convert template between ANSI-378 and ISO-19794-2

Matching
  match <a> <b> [--threshold T] [-v]  1:1 compare; a/b may be templates or images
  enroll   --db <file> --id <id> <template|image> [--dpi N]
  remove   --db <file> --id <id>
  identify --db <file> <template|image> [--top 10] [--threads N] [--threshold T]
  compact  --db <file>                Rewrite the store keeping only live records

Testing / benchmarking
  synth -o <out.bmp> [--finger N] [--impression K] [--width 320 --height 400]
                                      Generate a synthetic fingerprint image
  demo [--out DIR]                    End-to-end demo on synthetic fingers (BMP->WSQ->template->match)
  eval <dir> [--dpi N]                Accuracy (EER, FMR100, FMR1000) on an FVC-style folder
                                      (files named <finger>_<impression>.bmp|tif|pgm|wsq)
  eval --synthetic FxI                Same, on F synthetic fingers x I impressions (e.g. 20x4)
  bench [--gallery 100000] [--probes 20] [--threads N]
                                      1:N speed benchmark on synthetic templates

Common options: --dpi N (default 500 when the file has none), -v/--verbose,
  --matcher "key=value,..."  override matcher parameters (see include/fpx/matcher.hpp)
)";

MatcherConfig g_cfg;

struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;
    bool has(const std::string& k) const { return opt.count(k) != 0; }
    std::string get(const std::string& k, const std::string& d = "") const {
        auto it = opt.find(k);
        return it == opt.end() ? d : it->second;
    }
    int geti(const std::string& k, int d) const { return has(k) ? std::stoi(get(k)) : d; }
    double getd(const std::string& k, double d) const { return has(k) ? std::stod(get(k)) : d; }
};

Args parse_args(int argc, char** argv, int from) {
    static const char* flags[] = {"-v", "--verbose", "--help", "-h"};
    Args a;
    for (int i = from; i < argc; ++i) {
        std::string s = argv[i];
        bool is_flag = std::find(std::begin(flags), std::end(flags), s) != std::end(flags);
        if (s.size() > 1 && s[0] == '-' && !std::isdigit(static_cast<unsigned char>(s[1]))) {
            if (is_flag) {
                a.opt[s] = "1";
            } else {
                if (i + 1 >= argc) throw Error("option " + s + " needs a value");
                a.opt[s] = argv[++i];
            }
        } else {
            a.pos.push_back(s);
        }
    }
    if (a.has("-o")) a.opt["--out"] = a.opt["-o"];
    if (a.has("-v")) a.opt["--verbose"] = "1";
    if (a.has("--matcher")) g_cfg = parse_matcher_config(a.get("--matcher"));
    return a;
}

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Load a template from a template file, or extract one from an image file.
FingerTemplate load_any(const std::string& path, int dpi, std::vector<uint8_t>* record = nullptr) {
    auto bytes = read_file(path);
    if (looks_like_template(bytes)) {
        if (record) *record = bytes;
        return parse_template(bytes);
    }
    auto img = decode_image(bytes, dpi);
    auto r = extract(img);
    if (record) *record = r.record;
    return r.parsed;
}

void print_template(const FingerTemplate& t, bool verbose) {
    std::printf("format:          %s\n", to_string(t.format));
    std::printf("image size:      %u x %u px\n", t.width, t.height);
    std::printf("resolution:      %u x %u px/cm (~%d dpi)\n", t.xres, t.yres, int(std::lround(t.xres * 2.54)));
    std::printf("finger position: %u\n", t.finger_position);
    std::printf("finger quality:  %u\n", t.finger_quality);
    std::printf("minutiae:        %zu\n", t.minutiae.size());
    if (verbose) {
        std::printf("  #    x    y  angle(deg)  type         quality\n");
        int i = 0;
        for (const auto& m : t.minutiae)
            std::printf("%3d %4u %4u  %9.1f  %-11s  %u\n", i++, m.x, m.y, m.angle * 360.0 / 256.0,
                        m.type == MinutiaType::Ending ? "ending" : m.type == MinutiaType::Bifurcation ? "bifurcation" : "other",
                        m.quality);
    }
}

int cmd_info(const Args& a) {
    if (a.pos.size() != 1) throw Error("usage: fpx info <file>");
    auto bytes = read_file(a.pos[0]);
    if (looks_like_template(bytes)) {
        print_template(parse_template(bytes), a.has("--verbose"));
        return 0;
    }
    auto fmt = detect_image_format(bytes);
    auto img = decode_image(bytes, a.geti("--dpi", 500));
    std::printf("image format: %s\nsize:         %d x %d px\nresolution:   %d dpi\nfile size:    %zu bytes\n",
                to_string(fmt), img.width, img.height, img.dpi, bytes.size());
    return 0;
}

int cmd_convert(const Args& a) {
    if (a.pos.size() != 2) throw Error("usage: fpx convert <in> <out>");
    auto img = load_image(a.pos[0], a.geti("--dpi", 500));
    if (a.has("--dpi")) img.dpi = a.geti("--dpi", 500);
    save_image(a.pos[1], img, float(a.getd("--bitrate", 0.75)));
    std::printf("wrote %s (%d x %d, %d dpi)\n", a.pos[1].c_str(), img.width, img.height, img.dpi);
    return 0;
}

int cmd_extract(const Args& a) {
    if (a.pos.size() != 1 || !a.has("--out")) throw Error("usage: fpx extract <image> -o <out.tmpl> [--format iso|ansi]");
    auto img = load_image(a.pos[0], a.geti("--dpi", 500));
    if (a.has("--dpi")) img.dpi = a.geti("--dpi", 500);
    if (a.has("--wsq")) {
        write_file(a.get("--wsq"), wsq_encode(img, float(a.getd("--bitrate", 0.75))));
        std::printf("wrote %s\n", a.get("--wsq").c_str());
    }
    ExtractOptions o;
    o.format = parse_template_format(a.get("--format", "iso"));
    o.finger_position = uint8_t(a.geti("--finger", 0));
    double t0 = now_ms();
    auto r = extract(img, o);
    double t1 = now_ms();
    write_file(a.get("--out"), r.record);
    std::printf("wrote %s: %s, %zu minutiae, %zu bytes (%.1f ms)\n", a.get("--out").c_str(), to_string(o.format),
                r.parsed.minutiae.size(), r.record.size(), t1 - t0);
    return 0;
}

int cmd_totemplate(const Args& a) {
    if (a.pos.size() != 2 || !a.has("--format")) throw Error("usage: fpx totemplate <in> <out> --format iso|ansi");
    auto t = parse_template(read_file(a.pos[0]));
    auto f = parse_template_format(a.get("--format"));
    write_file(a.pos[1], serialize_template(t, f));
    std::printf("wrote %s (%s, %zu minutiae)\n", a.pos[1].c_str(), to_string(f), t.minutiae.size());
    return 0;
}

int cmd_match(const Args& a) {
    if (a.pos.size() != 2) throw Error("usage: fpx match <a> <b>");
    const int dpi = a.geti("--dpi", 500);
    auto ta = load_any(a.pos[0], dpi), tb = load_any(a.pos[1], dpi);
    Matcher m(g_cfg);
    MatchDetails d;
    double t0 = now_ms();
    float s = m.match(ta, tb, &d);
    double t1 = now_ms();
    const float thr = float(a.getd("--threshold", kDefaultThreshold));
    std::printf("score:     %.2f\nthreshold: %.2f\ndecision:  %s\n", s, thr, s >= thr ? "MATCH" : "NO MATCH");
    if (a.has("--verbose")) {
        std::printf("minutiae:  %zu vs %zu\npaired:    %d (overlap %d / %d)\nlocal corr: %d\n", ta.minutiae.size(),
                    tb.minutiae.size(), d.matched, d.probe_overlap, d.candidate_overlap, d.correspondences);
        std::printf("alignment: rot %.1f deg, shift (%.1f, %.1f)\ntime:      %.3f ms\n", d.rotation_deg, d.dx, d.dy,
                    t1 - t0);
    }
    return s >= thr ? 0 : 1;
}

// Gallery + store helpers ---------------------------------------------------
void load_gallery(TemplateStore& store, Gallery& g, bool quiet = false) {
    double t0 = now_ms();
    std::string warn;
    size_t bad = 0;
    store.replay(
        [&](const std::string& id, const std::vector<uint8_t>& rec) {
            try {
                g.insert(id, parse_template(rec));
            } catch (const Error&) {
                ++bad;
            }
        },
        [&](const std::string& id) { g.remove(id); }, &warn);
    if (!warn.empty()) std::fprintf(stderr, "warning: %s\n", warn.c_str());
    if (bad) std::fprintf(stderr, "warning: %zu undecodable templates skipped\n", bad);
    if (!quiet)
        std::fprintf(stderr, "loaded %zu templates from %s in %.0f ms\n", g.size(), store.path().c_str(), now_ms() - t0);
}

int cmd_enroll(const Args& a) {
    if (!a.has("--db") || !a.has("--id") || a.pos.size() != 1) throw Error("usage: fpx enroll --db F --id ID <file>");
    std::vector<uint8_t> rec;
    auto t = load_any(a.pos[0], a.geti("--dpi", 500), &rec);
    if (t.minutiae.size() < 3) throw Error("template has too few minutiae to be useful");
    TemplateStore store(a.get("--db"));
    store.put(a.get("--id"), rec);
    store.sync();
    std::printf("enrolled '%s' (%zu minutiae) into %s\n", a.get("--id").c_str(), t.minutiae.size(), a.get("--db").c_str());
    return 0;
}

int cmd_remove(const Args& a) {
    if (!a.has("--db") || !a.has("--id")) throw Error("usage: fpx remove --db F --id ID");
    TemplateStore store(a.get("--db"));
    store.erase(a.get("--id"));
    store.sync();
    std::printf("removed '%s'\n", a.get("--id").c_str());
    return 0;
}

int cmd_compact(const Args& a) {
    if (!a.has("--db")) throw Error("usage: fpx compact --db F");
    TemplateStore store(a.get("--db"));
    store.compact();
    std::printf("compacted %s\n", a.get("--db").c_str());
    return 0;
}

int cmd_identify(const Args& a) {
    if (!a.has("--db") || a.pos.size() != 1) throw Error("usage: fpx identify --db F <file>");
    auto probe = load_any(a.pos[0], a.geti("--dpi", 500));
    TemplateStore store(a.get("--db"));
    Gallery g(g_cfg);
    load_gallery(store, g);
    IdentifyOptions o;
    o.top_k = a.geti("--top", 10);
    o.threads = a.geti("--threads", 0);
    auto r = g.identify(probe, o);
    const float thr = float(a.getd("--threshold", kIdentifyThreshold));
    std::printf("searched %llu templates in %.1f ms\n", (unsigned long long)r.compared, r.elapsed_ms);
    std::printf("rank  score   decision  id\n");
    int rank = 1;
    for (auto& c : r.candidates)
        std::printf("%4d  %6.2f  %-8s  %s\n", rank++, c.score, c.score >= thr ? "MATCH" : "-", c.id.c_str());
    return (!r.candidates.empty() && r.candidates[0].score >= thr) ? 0 : 1;
}

int cmd_synth(const Args& a) {
    if (!a.has("--out")) throw Error("usage: fpx synth -o out.bmp [--finger N] [--impression K]");
    SynthImageOptions o;
    o.width = a.geti("--width", 320);
    o.height = a.geti("--height", 400);
    o.dpi = a.geti("--dpi", 500);
    auto img = synthesize_image(uint32_t(a.geti("--finger", 1)), uint32_t(a.geti("--impression", 0)), o);
    save_image(a.get("--out"), img);
    std::printf("wrote %s (%d x %d @ %d dpi)\n", a.get("--out").c_str(), img.width, img.height, img.dpi);
    return 0;
}

// Accuracy evaluation -----------------------------------------------------
struct Sample {
    std::string finger;
    int impression;
    FingerTemplate t;
};

struct Rates {
    double eer, eer_thr, fmr100_fnmr, fmr1000_fnmr;
};

Rates compute_rates(std::vector<float> gen, std::vector<float> imp) {
    std::sort(gen.begin(), gen.end());
    std::sort(imp.begin(), imp.end());
    auto fnmr = [&](float t) {  // genuine below threshold
        return double(std::lower_bound(gen.begin(), gen.end(), t) - gen.begin()) / gen.size();
    };
    auto fmr = [&](float t) {  // impostor at/above threshold
        return double(imp.end() - std::lower_bound(imp.begin(), imp.end(), t)) / imp.size();
    };
    Rates r{1, 0, 1, 1};
    double best = 2;
    for (float t = 0; t <= 100.f; t += 0.25f) {
        double a = fnmr(t), b = fmr(t);
        if (std::abs(a - b) < best) {
            best = std::abs(a - b);
            r.eer = (a + b) / 2;
            r.eer_thr = t;
        }
        if (b <= 0.01 && a < r.fmr100_fnmr) r.fmr100_fnmr = a;
        if (b <= 0.001 && a < r.fmr1000_fnmr) r.fmr1000_fnmr = a;
    }
    return r;
}

void report_eval(const std::vector<Sample>& samples, double extract_ms, size_t failed, bool verbose) {
    Matcher m(g_cfg);
    std::vector<float> gen, imp;
    std::vector<std::pair<size_t, size_t>> gp, ip;
    std::map<std::string, size_t> first;
    for (size_t i = 0; i < samples.size(); ++i)
        if (!first.count(samples[i].finger)) first[samples[i].finger] = i;
    for (size_t i = 0; i < samples.size(); ++i)
        for (size_t j = i + 1; j < samples.size(); ++j)
            if (samples[i].finger == samples[j].finger) gp.push_back({i, j});
    for (auto a = first.begin(); a != first.end(); ++a)  // FVC protocol: first impressions vs each other
        for (auto b = std::next(a); b != first.end(); ++b) ip.push_back({a->second, b->second});
    double t0 = now_ms();
    for (auto& p : gp) gen.push_back(m.match(samples[p.first].t, samples[p.second].t));
    for (auto& p : ip) imp.push_back(m.match(samples[p.first].t, samples[p.second].t));
    double t1 = now_ms();
    if (gen.empty() || imp.empty()) throw Error("need at least 2 fingers with 2 impressions each");
    if (verbose) {  // show the hardest cases - useful when tuning
        std::vector<size_t> oi(imp.size()), og(gen.size());
        for (size_t i = 0; i < oi.size(); ++i) oi[i] = i;
        for (size_t i = 0; i < og.size(); ++i) og[i] = i;
        std::sort(oi.begin(), oi.end(), [&](size_t x, size_t y) { return imp[x] > imp[y]; });
        std::sort(og.begin(), og.end(), [&](size_t x, size_t y) { return gen[x] < gen[y]; });
        auto show = [&](const char* what, const std::pair<size_t, size_t>& pr, float sc) {
            MatchDetails d;
            m.match(samples[pr.first].t, samples[pr.second].t, &d);
            std::printf("  %s %s_%d vs %s_%d: score %.1f, minutiae %zu/%zu, paired %d, supported %d, overlap %d/%d, corr %d\n",
                        what, samples[pr.first].finger.c_str(), samples[pr.first].impression,
                        samples[pr.second].finger.c_str(), samples[pr.second].impression, sc,
                        samples[pr.first].t.minutiae.size(), samples[pr.second].t.minutiae.size(), d.paired, d.matched,
                        d.probe_overlap, d.candidate_overlap, d.correspondences);
        };
        for (size_t k = 0; k < std::min<size_t>(5, oi.size()); ++k) show("impostor", ip[oi[k]], imp[oi[k]]);
        for (size_t k = 0; k < std::min<size_t>(5, og.size()); ++k) show("genuine ", gp[og[k]], gen[og[k]]);
    }
    Rates r = compute_rates(gen, imp);
    auto mean = [](const std::vector<float>& v) {
        double s = 0;
        for (float x : v) s += x;
        return s / v.size();
    };
    std::printf("\nsamples: %zu (failed to extract: %zu), avg extraction %.1f ms\n", samples.size(), failed, extract_ms);
    std::printf("genuine comparisons:  %zu  (mean score %.1f)\n", gen.size(), mean(gen));
    std::printf("impostor comparisons: %zu  (mean score %.2f, max %.2f)\n", imp.size(), mean(imp),
                *std::max_element(imp.begin(), imp.end()));
    std::printf("avg 1:1 match time:   %.3f ms\n", (t1 - t0) / double(gen.size() + imp.size()));
    std::printf("EER:                  %.2f %% (threshold %.2f)\n", r.eer * 100, r.eer_thr);
    std::printf("FNMR @ FMR=1%%:        %.2f %%\n", r.fmr100_fnmr * 100);
    std::printf("FNMR @ FMR=0.1%%:      %.2f %%\n", r.fmr1000_fnmr * 100);
    size_t gfail = size_t(std::count_if(gen.begin(), gen.end(), [](float s) { return s < kDefaultThreshold; }));
    size_t ipass = size_t(std::count_if(imp.begin(), imp.end(), [](float s) { return s >= kDefaultThreshold; }));
    std::printf("at default threshold %.1f: FNMR %.2f %%, FMR %.3f %%\n", kDefaultThreshold, 100.0 * gfail / gen.size(),
                100.0 * ipass / imp.size());
}

int cmd_eval(const Args& a) {
    std::vector<Sample> samples;
    size_t failed = 0;
    double ext_ms = 0;
    std::mutex mu;
    if (a.has("--synthetic")) {
        std::string spec = a.get("--synthetic");
        auto x = spec.find('x');
        if (x == std::string::npos) throw Error("--synthetic expects FxI, e.g. 20x4");
        int F = std::stoi(spec.substr(0, x)), I = std::stoi(spec.substr(x + 1));
        std::vector<std::pair<int, int>> jobs;
        for (int f = 1; f <= F; ++f)
            for (int i = 0; i < I; ++i) jobs.push_back({f, i});
        std::atomic<size_t> next{0};
        std::vector<std::thread> th;
        double t0 = now_ms();
        for (unsigned w = 0; w < std::max(1u, std::thread::hardware_concurrency()); ++w)
            th.emplace_back([&] {
                for (size_t k; (k = next++) < jobs.size();) {
                    auto img = synthesize_image(uint32_t(jobs[k].first), uint32_t(jobs[k].second));
                    try {
                        auto t = extract(img).parsed;
                        std::lock_guard<std::mutex> lk(mu);
                        samples.push_back({std::to_string(jobs[k].first), jobs[k].second, std::move(t)});
                    } catch (const Error&) {
                        std::lock_guard<std::mutex> lk(mu);
                        ++failed;
                    }
                }
            });
        for (auto& t : th) t.join();
        ext_ms = (now_ms() - t0) / double(jobs.size());
        std::sort(samples.begin(), samples.end(), [](const Sample& p, const Sample& q) {
            return std::stoi(p.finger) != std::stoi(q.finger) ? std::stoi(p.finger) < std::stoi(q.finger)
                                                              : p.impression < q.impression;
        });
        std::printf("synthetic dataset: %d fingers x %d impressions (synthesis + extraction)\n", F, I);
    } else {
        if (a.pos.size() != 1) throw Error("usage: fpx eval <dir> | fpx eval --synthetic 20x4");
        const int dpi = a.geti("--dpi", 500);
        double total = 0;
        for (auto& e : fs::directory_iterator(a.pos[0])) {
            if (!e.is_regular_file()) continue;
            std::string stem = e.path().stem().string();
            auto us = stem.rfind('_');
            if (us == std::string::npos) continue;
            try {
                auto bytes = read_file(e.path().string());
                FingerTemplate t;
                if (looks_like_template(bytes)) {
                    t = parse_template(bytes);
                } else {
                    auto img = decode_image(bytes, dpi);
                    if (a.has("--dpi")) img.dpi = dpi;
                    double t0 = now_ms();
                    t = extract(img).parsed;
                    total += now_ms() - t0;
                }
                samples.push_back({stem.substr(0, us), std::stoi(stem.substr(us + 1)), std::move(t)});
            } catch (const std::exception& ex) {
                ++failed;
                if (a.has("--verbose")) std::fprintf(stderr, "skip %s: %s\n", e.path().c_str(), ex.what());
            }
        }
        ext_ms = samples.empty() ? 0 : total / samples.size();
        std::sort(samples.begin(), samples.end(), [](const Sample& p, const Sample& q) {
            return p.finger != q.finger ? p.finger < q.finger : p.impression < q.impression;
        });
    }
    report_eval(samples, ext_ms, failed, a.has("--verbose"));
    return 0;
}

int cmd_demo(const Args& a) {
    const std::string dir = a.get("--out", "fpx-demo");
    fs::create_directories(dir);
    std::printf("== 1. Synthesising 3 fingers x 2 impressions (stand-in for scanner bitmaps)\n");
    std::vector<std::string> names;
    for (int f = 1; f <= 3; ++f)
        for (int i = 0; i < 2; ++i) {
            std::string n = dir + "/finger" + std::to_string(f) + "_" + std::to_string(i);
            save_image(n + ".bmp", synthesize_image(uint32_t(f), uint32_t(i)));
            names.push_back(n);
        }
    std::printf("== 2. BMP -> WSQ -> decoded image -> ISO + ANSI templates\n");
    std::vector<FingerTemplate> tmpls;
    for (auto& n : names) {
        auto img = load_image(n + ".bmp");
        auto wsq = wsq_encode(img);
        write_file(n + ".wsq", wsq);
        auto back = wsq_decode(wsq);
        ExtractOptions o;
        auto iso = extract(back, o);
        write_file(n + ".iso", iso.record);
        o.format = TemplateFormat::Ansi378_2004;
        write_file(n + ".ansi", extract(back, o).record);
        std::printf("   %-26s bmp %6zu B  wsq %6zu B  minutiae %3zu\n", (n + ".bmp").c_str(), fs::file_size(n + ".bmp"),
                    wsq.size(), iso.parsed.minutiae.size());
        tmpls.push_back(iso.parsed);
    }
    std::printf("== 3. Score matrix (genuine pairs on the diagonal blocks), threshold %.1f\n        ", kDefaultThreshold);
    for (size_t j = 0; j < tmpls.size(); ++j) std::printf("  f%zu_%zu", j / 2 + 1, j % 2);
    std::printf("\n");
    Matcher m(g_cfg);
    for (size_t i = 0; i < tmpls.size(); ++i) {
        std::printf("   f%zu_%zu ", i / 2 + 1, i % 2);
        for (size_t j = 0; j < tmpls.size(); ++j) std::printf(" %6.1f", m.match(tmpls[i], tmpls[j]));
        std::printf("\n");
    }
    std::printf("== 4. 1:N: enrol impression 0 of each finger, identify impression 1\n");
    Gallery g(g_cfg);
    for (size_t i = 0; i < tmpls.size(); i += 2) g.insert("finger" + std::to_string(i / 2 + 1), tmpls[i]);
    for (size_t i = 1; i < tmpls.size(); i += 2) {
        auto r = g.identify(tmpls[i], {3, 0, 0});
        std::printf("   probe finger%zu_1 -> %s (score %.1f)\n", i / 2 + 1,
                    r.candidates.empty() ? "none" : r.candidates[0].id.c_str(),
                    r.candidates.empty() ? 0.f : r.candidates[0].score);
    }
    std::printf("files written to %s/\n", dir.c_str());
    return 0;
}

int cmd_bench(const Args& a) {
    const size_t N = size_t(a.geti("--gallery", 100000));
    const int P = a.geti("--probes", 20);
    const int threads = a.geti("--threads", 0);
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::printf("building synthetic gallery of %zu templates on %u threads...\n", N, hw);
    Gallery g(g_cfg);
    g.reserve(N, 40);
    double t0 = now_ms();
    {
        std::atomic<size_t> next{0};
        std::vector<std::thread> th;
        const size_t batch = 1024;
        for (unsigned w = 0; w < hw; ++w)
            th.emplace_back([&] {
                std::vector<std::pair<std::string, CompiledTemplate>> buf;
                for (;;) {
                    size_t b = next.fetch_add(batch);
                    if (b >= N) break;
                    buf.clear();
                    for (size_t i = b; i < std::min(N, b + batch); ++i)
                        buf.push_back({"subject-" + std::to_string(i), compile(synthesize_template(i))});
                    for (auto& x : buf) g.insert(x.first, x.second);
                }
            });
        for (auto& t : th) t.join();
    }
    double t1 = now_ms();
    std::printf("gallery ready: %zu templates, ~%.0f MB, built in %.1f s\n", g.size(), g.memory_bytes() / 1048576.0,
                (t1 - t0) / 1000);

    IdentifyOptions o;
    o.top_k = 5;
    o.threads = threads;
    int hits = 0;
    double total = 0, worst = 0;
    std::vector<float> gen_scores, imp_best;
    for (int p = 0; p < P; ++p) {
        size_t target = size_t((uint64_t(p) * 2654435761u) % N);
        auto probe = perturb_template(synthesize_template(target), 1000 + p);
        auto r = g.identify(probe, o);
        total += r.elapsed_ms;
        worst = std::max(worst, r.elapsed_ms);
        bool hit = !r.candidates.empty() && r.candidates[0].id == "subject-" + std::to_string(target) &&
                   r.candidates[0].score >= kIdentifyThreshold;
        hits += hit;
        if (hit) gen_scores.push_back(r.candidates[0].score);
        if (a.has("--verbose"))
            std::printf("  probe %2d target %-8zu -> %-16s score %6.2f  %.0f ms\n", p, target,
                        r.candidates.empty() ? "-" : r.candidates[0].id.c_str(),
                        r.candidates.empty() ? 0.f : r.candidates[0].score, r.elapsed_ms);
    }
    // Impostor searches: fingers that are not enrolled.
    double imp_total = 0;
    for (int p = 0; p < std::max(1, P / 4); ++p) {
        auto probe = perturb_template(synthesize_template(N + 1000000 + size_t(p)), 7 + p);
        auto r = g.identify(probe, o);
        imp_total += r.elapsed_ms;
        imp_best.push_back(r.candidates.empty() ? 0.f : r.candidates[0].score);
        if (a.has("--verbose") && !r.candidates.empty()) {
            size_t cid = std::stoul(r.candidates[0].id.substr(8));
            MatchDetails d;
            Matcher mm(g_cfg);
            float sc = mm.match(Probe(probe, g_cfg), compile(synthesize_template(cid), g_cfg).view(), &d);
            std::printf("  impostor probe %d -> %s score %.1f: minutiae %zu, paired %d, supported %d, overlap %d/%d, corr %d\n",
                        p, r.candidates[0].id.c_str(), sc, probe.minutiae.size(), d.paired, d.matched, d.probe_overlap,
                        d.candidate_overlap, d.correspondences);
        }
    }
    const double avg = total / P;
    std::printf("\n1:N search over %zu templates (threads=%d)\n", N, threads > 0 ? threads : int(hw));
    std::printf("  avg latency:      %.1f ms   (worst %.1f ms)\n", avg, worst);
    std::printf("  throughput:       %.2f M comparisons/s\n", N / avg / 1000.0);
    std::printf("  rank-1 hit rate:  %d / %d  (correct id AND score >= identify threshold %.0f)\n", hits, P,
                kIdentifyThreshold);
    if (!gen_scores.empty())
        std::printf("  genuine score:    min %.1f  mean %.1f   (over the hits)\n", *std::min_element(gen_scores.begin(), gen_scores.end()),
                    [&] { double s = 0; for (float x : gen_scores) s += x; return s / gen_scores.size(); }());
    const float imp_max = *std::max_element(imp_best.begin(), imp_best.end());
    std::printf("  best impostor score over the whole gallery (%zu non-enrolled probes): max %.1f -> %s\n",
                imp_best.size(), imp_max, imp_max < kIdentifyThreshold ? "no false alarm" : "FALSE ALARM");
    std::printf("  projected time for 1,000,000 templates on this machine: %.2f s\n", avg * 1e6 / N / 1000.0);
    (void)imp_total;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h" || std::string(argv[1]) == "help") {
        std::fputs(kUsage, stdout);
        return argc < 2 ? 2 : 0;
    }
    const std::string cmd = argv[1];
    if (cmd == "--version" || cmd == "version") {
        std::printf("fpx %s\n", FPX_VERSION_STRING);
        return 0;
    }
    try {
        Args a = parse_args(argc, argv, 2);
        if (cmd == "info") return cmd_info(a);
        if (cmd == "convert") return cmd_convert(a);
        if (cmd == "extract") return cmd_extract(a);
        if (cmd == "totemplate") return cmd_totemplate(a);
        if (cmd == "match") return cmd_match(a);
        if (cmd == "enroll") return cmd_enroll(a);
        if (cmd == "remove") return cmd_remove(a);
        if (cmd == "compact") return cmd_compact(a);
        if (cmd == "identify") return cmd_identify(a);
        if (cmd == "synth") return cmd_synth(a);
        if (cmd == "demo") return cmd_demo(a);
        if (cmd == "eval") return cmd_eval(a);
        if (cmd == "bench") return cmd_bench(a);
        std::fprintf(stderr, "unknown command '%s'\n\n%s", cmd.c_str(), kUsage);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 3;
    }
}
