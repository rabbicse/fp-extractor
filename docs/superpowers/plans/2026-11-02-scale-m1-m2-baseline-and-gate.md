# Scale Milestones 1-2: Baseline, Variant Lab and Go/No-Go Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Document the system as it exists, record measured baselines, and build the variant lab and bit-signature experiment that decide whether the CPU cascade can proceed.

**Architecture:** The signature ships in `fpx_core` because later milestones use it. Everything experimental lives in a new `fpx_lab` static library with an `fpx-lab` executable that runs one variant and emits JSON. A Python runner expands `tests/variants/matrix.yaml`, calls `fpx-lab`, and aggregates results. The server and gallery are not changed.

**Tech Stack:** C++17, CMake, doctest, nlohmann/json (already fetched by `cmake/Dependencies.cmake`), Python 3.9+, PyYAML and allpairspy (pinned in `tools/lab/requirements.txt`), pytest.

**Spec:** `docs/superpowers/specs/2026-11-02-fp-extractor-scale-design.md` (sections 2, 5, 6, 12, 13, 16, 17)

Milestones 3-7 get their own plans after the gate result in Task 9.

## Global Constraints

- Work on branch `feature/scale-m1-m2`, created from `design/scale-platform`.
- Gate pass bar: at least 99.5% of full-scan matches survive. Filter rates: 99% and 99.9%.
- Signature sizes: 32, 64 and 128 bytes.
- Decision thresholds stay `kVerifyThreshold` (12) and `kIdentifyThreshold` (18) unless a report recommends otherwise. This plan changes no defaults.
- Full matrix: 4 scanner pairs × 5 paths × 2 formats × 2 finger counts × 7 search modes × 6 conditions = 3,360 variants. Without scanner pairs: 840.
- MCC must not be used. The signature encoding is our own.
- Real scans and public datasets never enter git and never run in CI. Corpus paths inside the repository are refused.
- Every reported number carries an evidence label: `measured`, `synthetic` or `projected`. Combined real and synthetic galleries report both counts.
- Skipped variants are recorded with a reason, never dropped silently.
- No conversion path or scanner pairing is declared the default in this plan.
- Docs describe what exists today as current, and the spec's design as planned. They must not present planned features as working.

## Review Focus

- A corpus file that is corrupt, blank or not a fingerprint: counted as failure to enrol in the results, and the run continues. Test in Task 3.
- A template with very few minutiae: its sparse signature must not rank near the top for every probe. Test in Task 4.
- An impression compared with itself: never counted as a genuine pair, since it would inflate survival. Test in Task 6.
- A full-matrix run interrupted partway: rerunning resumes and does not repeat or corrupt finished variants. Test in Task 7.
- An ANSI enrolment searched with an ISO probe: scores within the same variant are valid, and mixed formats do not fail. Test in Task 6.

---

### Task 1: Current-state documentation

**Files:**
- Modify: `docs/ARCHITECTURE.md`, `docs/SCALING.md`, `README.md`
- Create: `docs/GETTING_STARTED.md`, `docs/ROADMAP.md`
- Test: `tests/check_docs.py`

**Interfaces:**
- Produces: `tests/check_docs.py <repo_root>`, exit 0 when all relative Markdown links resolve and each command in fenced `fpx-check` blocks exits 0.

- [ ] **Step 1: Write the failing check**

`tests/check_docs.py` scans `README.md` and `docs/*.md`, excluding `docs/superpowers/`. Register as ctest `docs_check`. It must fail if `docs/GETTING_STARTED.md` is missing, and if `README.md` still contains `feature/identification-engine`.

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 tests/check_docs.py .`
Expected: FAIL naming `docs/GETTING_STARTED.md` and the stale branch reference.

- [ ] **Step 3: Write the docs**

- `docs/ARCHITECTURE.md`: Mermaid component, enrol, identify and failure-today diagrams of the current system. A short "Planned" section links to the spec and `docs/ROADMAP.md`.
- `docs/GETTING_STARTED.md`: prerequisites, build, test, `fpx demo`, extract and match, server start, and enrol and identify with `examples/python/client.py`. Commands from `docs/TESTING.md` sections 1, 2 and 5 are the source, verified by running them.
- `docs/ROADMAP.md`: the 7 milestones from spec section 13, each with status.
- `docs/SCALING.md`: add a banner saying its figures are from one 4-vCPU run, that searches run one at a time per process, and linking the spec. Add the Section 2 M1 results labelled `measured`, and replace "the target is met" wording with what was measured.
- `README.md`: point clone instructions at `master`, and link the new docs.

- [ ] **Step 4: Run the check and the guide**

Run: `python3 tests/check_docs.py .`
Expected: PASS. Then follow `docs/GETTING_STARTED.md` from a fresh build directory and confirm every step works on the M1.

- [ ] **Step 5: Commit**

```bash
git add README.md docs tests/check_docs.py tests/CMakeLists.txt
git commit -m "docs: document current architecture and getting started"
```

### Task 2: Machine-readable baselines

**Files:**
- Modify: `src/cli/main.cpp` (`cmd_bench`, `cmd_bench_two_fingers`, `cmd_eval`), `tests/CMakeLists.txt`
- Create: `tools/lab/baseline.py`, `tests/lab/test_baseline.py`, `benchmarks/README.md`

**Interfaces:**
- Produces: `fpx bench ... --json PATH` and `fpx eval ... --json PATH`. Bench keys: `gallery`, `templates`, `fingers`, `threads`, `avg_latency_ms`, `worst_latency_ms`, `comparisons_per_s`, `rank1_hits`, `probes`, `best_impostor_score`, `memory_mb`. Eval keys: `genuine_pairs`, `impostor_pairs`, `eer`, `fnmr_at_fmr_1pct`, `fnmr_at_fmr_0_1pct`, `failed`.
- Produces: `tools/lab/baseline.py --fpx PATH --out benchmarks/baselines/<host>-<YYYY-MM-DD>.json`, adding `git_sha`, `cpu`, `cores`, `compiler` and `evidence: "synthetic"`.

- [ ] **Step 1: Write the failing test**

```python
def test_bench_json_has_required_keys(fpx, tmp_path):
    out = tmp_path / "b.json"
    run([fpx, "bench", "--gallery", "2000", "--probes", "3", "--json", out])
    data = json.loads(out.read_text())
    assert data["gallery"] == 2000 and data["probes"] == 3
    assert data["avg_latency_ms"] > 0 and data["comparisons_per_s"] > 0

def test_baseline_records_git_sha_and_evidence(fpx, tmp_path):
    data = run_baseline(fpx, tmp_path, quick=True)
    assert len(data["git_sha"]) >= 7 and data["evidence"] == "synthetic"
```

- [ ] **Step 2: Run to verify it fails**

Run: `FPX_BIN=build/fpx pytest tests/lab/test_baseline.py -v`
Expected: FAIL, because `--json` is not recognised.

- [ ] **Step 3: Implement the `--json` flags and `baseline.py`**

Existing text output is unchanged. `baseline.py` runs bench at 100k and 1M one-finger, 500k two-finger, and `eval --synthetic 50x4`. `--quick` uses 2,000, 2,000, 1,000 and `10x4`.

- [ ] **Step 4: Run tests, then record the real baseline**

Run: `FPX_BIN=build/fpx pytest tests/lab/test_baseline.py -v`
Expected: PASS.
Run: `python3 tools/lab/baseline.py --fpx build/fpx --out benchmarks/baselines/m1-$(date +%F).json`
Expected: file written with all keys. Peak RAM stays under 16 GB.

- [ ] **Step 5: Commit**

```bash
git add src/cli/main.cpp tools/lab tests benchmarks
git commit -m "feat: add JSON benchmark output and record baseline"
```

### Task 3: Corpus manifest and mock data generator

**Files:**
- Create: `include/fpx/lab/corpus.hpp`, `src/lab/corpus.cpp`, `include/fpx/lab/mock.hpp`, `src/lab/mock.cpp`, `tests/test_lab_corpus.cpp`, `docs/CORPUS.md`
- Modify: `CMakeLists.txt` (add `fpx_lab`, linked to `fpx_core` and `nlohmann_json`; link `fpx_cli` to it), `src/cli/main.cpp` (add `mockgen`), `tests/CMakeLists.txt`

**Interfaces:**
- Produces, in namespace `fpx::lab`:

```cpp
enum class Condition { Normal, Dry, Wet, Partial, Rotated, LightPressure };
struct CorpusItem { std::string path, subject, scanner; uint8_t position; int impression; Condition condition; int dpi; };
struct LoadIssue { std::string path, reason; };
struct Corpus { std::vector<CorpusItem> items; std::vector<LoadIssue> issues; };
Corpus load_manifest(const std::string& csv_path, const std::string& repo_root);
GrayImage apply_condition(const GrayImage&, Condition, uint64_t seed);

struct MockOptions { int people, fingers, impressions; int width, height, dpi; std::string scanner; std::vector<Condition> conditions; uint64_t seed; };
Corpus generate_images(const MockOptions&, const std::string& out_dir);
void generate_templates(uint64_t count, uint64_t seed, const std::string& out_path);
```

- Produces: `fpx mockgen --people N --fingers F --impressions I --width W --height H --dpi D --scanner NAME --conditions a,b --seed S --out DIR`, and `--templates N` for template-only output. Writes `<subject>_<position>_<impression>.{bmp,wsq,ansi,iso}` and `manifest.csv` with columns `path,subject,scanner,position,impression,condition,dpi`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("manifest refuses a corpus path inside the repository") {
    CHECK_THROWS_AS(load_manifest(write_csv(repo_root + "/corpus.csv"), repo_root), Error);
}
TEST_CASE("bad manifest rows become issues, not crashes") {
    auto c = load_manifest(csv_with_bad_position_and_missing_file(), "/nonexistent-root");
    CHECK(c.issues.size() == 2);
}
TEST_CASE("mock data is reproducible from its seed") {
    CHECK(file_hashes(mock(seed = 7)) == file_hashes(mock(seed = 7)));
    CHECK(file_hashes(mock(seed = 7)) != file_hashes(mock(seed = 8)));
}
TEST_CASE("mock manifest lists every format of every impression") {
    auto c = generate_images({2, 2, 3, 320, 400, 500, "mock", {Condition::Normal}, 1}, dir);
    CHECK(c.items.size() == 2 * 2 * 3 * 4);
}
TEST_CASE("conditions change the image, except Normal") {
    CHECK(apply_condition(img, Condition::Normal, 1).pixels == img.pixels);
    CHECK(apply_condition(img, Condition::Partial, 1).pixels != img.pixels);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build -j && build/tests/fpx_tests -tc="*manifest*,*mock*,*conditions*"`
Expected: build failure on `fpx/lab/corpus.hpp`.

- [ ] **Step 3: Implement corpus, mock and `mockgen`**

Use `synthesize_image` and the BMP writer used by `cmd_demo`. Conditions, applied after synthesis: Dry lowers contrast and breaks ridges with seeded speckle. Wet thickens ridges with a blur and darkens. Partial blanks 40% of the image from a seeded edge. Rotated uses `rotation_deg = 20`. LightPressure raises brightness and lowers contrast. These are approximations, and `docs/CORPUS.md` says so.

`docs/CORPUS.md` covers the capture protocol from spec section 16, consent, encryption, storage outside git, and the manifest format. Scanner width, height and DPI are flags rather than hardcoded values until exact models are known.

- [ ] **Step 4: Run tests**

Run: `build/tests/fpx_tests -tc="*manifest*,*mock*,*conditions*"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt include/fpx/lab src/lab src/cli/main.cpp tests docs/CORPUS.md
git commit -m "feat: add corpus manifest and mock data generator"
```

### Task 4: Bit signature

**Files:**
- Create: `include/fpx/signature.hpp`, `src/core/signature.cpp`, `tests/test_signature.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `fpx::CompiledTemplate`, `fpx::CEdge` from `fpx/matcher.hpp`.
- Produces:

```cpp
namespace fpx {
struct SignatureConfig {
    size_t bytes = 64;      // 32, 64 or 128; must be a multiple of 8
    int d_shift = 3, a_shift = 4, b_shift = 4;  // quantisation of CEdge d, a, b
    int spread = 1;         // neighbouring cells also set, to absorb tolerance
    int target_bits_pct = 25; // bit density the encoding aims for
};
void compute_signature(const CompiledTemplate&, const SignatureConfig&, uint64_t* out_words);
uint32_t signature_distance(const uint64_t* a, const uint64_t* b, size_t words);
}
```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("signature is deterministic and fits its size") { /* same input gives identical words */ }
TEST_CASE("signature is invariant to rotation and translation") {
    // perturb_template with position_jitter=0, angle_jitter=0, drop_rate=0, spurious_rate=0
    CHECK(distance(sig(t), sig(rotated_shifted(t))) <= bits(64) / 20);
}
TEST_CASE("genuine pairs are closer than impostor pairs") {
    // 500 synthetic fingers, default perturb_template
    CHECK(mean_distance(genuine) < mean_distance(impostor) * 0.8);
}
TEST_CASE("sparse template does not rank near the top for every probe") {
    // template with 6 minutiae against 200 ordinary probes, in a 2000-template gallery
    CHECK(fraction_of_probes_with_sparse_in_top_1pct < 0.05);
}
TEST_CASE("rejects sizes that are not a multiple of 8") { /* throws fpx::Error */ }
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build -j`
Expected: build failure on `fpx/signature.hpp`.

- [ ] **Step 3: Implement**

For each edge, quantise `(d >> d_shift, a >> a_shift, b >> b_shift)`. Hash the cell, plus `±spread` neighbours in `d`, to bit positions with a fixed-seed mix. `a` and `b` wrap around, because they are angles. To control density, and so that sparse templates are not systematically close, hash each edge with `h` functions chosen from edge count to reach `target_bits_pct`, then top up or thin deterministically to exactly that bit count. Distance uses `__builtin_popcountll` over 64-bit words, falling back to a portable popcount under MSVC.

- [ ] **Step 4: Run tests**

Run: `build/tests/fpx_tests -tc="*signature*"`
Expected: PASS. If the genuine and impostor test fails at the default config, sweep the config fields before changing the test, and report the result.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt include/fpx/signature.hpp src/core/signature.cpp tests
git commit -m "feat: add rotation-invariant bit signature"
```

### Task 5: Metrics and gate verdict

**Files:**
- Create: `include/fpx/lab/metrics.hpp`, `src/lab/metrics.cpp`, `tests/test_lab_metrics.cpp`
- Modify: `src/cli/main.cpp` (use shared `compute_rates`)

**Interfaces:**
- Produces, in `fpx::lab`:

```cpp
struct ErrorRates { double eer, eer_threshold, fnmr_fmr_1, fnmr_fmr_0_1, fnmr_fmr_0_01; bool fmr_1_supported, fmr_0_1_supported, fmr_0_01_supported; };
ErrorRates compute_rates(std::vector<float> genuine, std::vector<float> impostor);
struct Survival { uint64_t full_scan_matches, survived; double pct() const; };
enum class GateResult { Pass, PassAt99Only, Stop, NotEvaluated };
struct GateInput { Survival at_99, at_99_9; uint64_t real_templates, synthetic_distractors; };
struct GateVerdict { GateResult result; bool provisional; std::string reason; };
GateVerdict gate_verdict(const GateInput&);
```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("gate boundaries") {
    CHECK(verdict(995, 1000, 995, 1000).result == GateResult::Pass);       // exactly 99.5%
    CHECK(verdict(995, 1000, 994, 1000).result == GateResult::PassAt99Only);
    CHECK(verdict(994, 1000, 900, 1000).result == GateResult::Stop);
}
TEST_CASE("gate is not evaluated without full-scan matches") {
    CHECK(verdict(0, 0, 0, 0).result == GateResult::NotEvaluated);
}
TEST_CASE("synthetic distractors make a pass provisional") {
    CHECK(verdict_with(800, 1000000).provisional);
    CHECK_FALSE(verdict_with(2000000, 0).provisional);
}
TEST_CASE("synthetic-only input is never a gate pass") {
    CHECK(verdict_with(0, 1000000).result == GateResult::NotEvaluated);
}
TEST_CASE("an FMR point needs at least 3/FMR impostor pairs") {
    auto r = compute_rates(genuine_100, impostors(1000));
    CHECK(r.fmr_1_supported);
    CHECK_FALSE(r.fmr_0_01_supported);  // needs 30000
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build -j`
Expected: build failure on `fpx/lab/metrics.hpp`.

- [ ] **Step 3: Implement**

Move the existing logic from `compute_rates` at `src/cli/main.cpp:357` and keep `fpx eval` output unchanged. Compare using integers (`survived * 1000 >= matches * 995`) to avoid floating-point errors at the boundary.

- [ ] **Step 4: Run tests, including existing suites**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass, including `cli_demo`.

- [ ] **Step 5: Commit**

```bash
git add include/fpx/lab/metrics.hpp src/lab/metrics.cpp src/cli/main.cpp tests CMakeLists.txt
git commit -m "feat: add error-rate metrics and gate verdict"
```

### Task 6: Single-variant runner

**Files:**
- Create: `include/fpx/lab/variant.hpp`, `src/lab/variant.cpp`, `src/lab/main.cpp`, `tests/test_lab_variant.cpp`
- Modify: `CMakeLists.txt` (`fpx_lab_cli`, output name `fpx-lab`), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `load_manifest`, `apply_condition` (Task 3), `compute_signature` (Task 4), `compute_rates`, `Survival` (Task 5), `fpx::extract`, `wsq_encode`, `wsq_decode`, `Matcher`, `Probe`.
- Produces:

```cpp
namespace fpx::lab {
enum class Path { BmpDirect, Wsq075, Wsq225, ServerSide, VendorTemplate };
struct SearchMode { bool full_scan; size_t signature_bytes; double filter_rate; };
struct VariantSpec { std::string enrol_scanner, probe_scanner; Path path; TemplateFormat format; int fingers; SearchMode search; Condition condition; uint64_t distractors; uint64_t seed; };
struct VariantResult { /* serialised below */ };
VariantResult run_variant(const VariantSpec&, const Corpus&);
}
```

- Produces: `fpx-lab run --manifest M --variant V.json --out R.json`. Result JSON keys: `variant`, `status` (`ok` or `skipped`), `skip_reason`, `evidence`, `rates`, `survival`, `failure_to_enrol`, `issues`, `stage1_per_core_per_s`, `stage2_per_core_per_s`, `avg_wsq_bytes`, `avg_template_bytes`, `real_templates`, `synthetic_distractors`, `git_sha`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("impressions are never compared with themselves") {
    auto r = run_variant(spec_full_scan(), mock_corpus(/*people*/5, /*fingers*/1, /*impressions*/4));
    CHECK(r.genuine_pairs == 5 * (4 * 3 / 2));
}
TEST_CASE("a corrupt corpus file counts as failure to enrol and the run continues") {
    CHECK(run_variant(spec_full_scan(), corpus_with_garbage_file()).failure_to_enrol == 1);
}
TEST_CASE("ServerSide equals Wsq075 byte for byte") { /* same record from the same image */ }
TEST_CASE("VendorTemplate without vendor records is skipped with a reason") {
    auto r = run_variant(spec_with_path(Path::VendorTemplate), mock_corpus(2, 1, 2));
    CHECK(r.status == "skipped");
    CHECK_FALSE(r.skip_reason.empty());
}
TEST_CASE("mixed ANSI enrolment and ISO probe scores the same as ISO only") {
    CHECK(score(ansi_enrol, iso_probe) == doctest::Approx(score(iso_enrol, iso_probe)).epsilon(0.01));
}
TEST_CASE("cascade at filter 0 finds everything the full scan finds") {
    CHECK(run_variant(spec_cascade(64, 0.0), corpus).survival.pct() == doctest::Approx(100.0));
}
TEST_CASE("wrong DPI shows up as worse results") {
    CHECK(run_variant(spec_full_scan(), corpus_labelled_250dpi()).rates.eer >
          run_variant(spec_full_scan(), corpus_correct_dpi()).rates.eer);
}
TEST_CASE("synthetic corpus is labelled synthetic") { /* evidence == "synthetic" */ }
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build -j`
Expected: build failure on `fpx/lab/variant.hpp`.

- [ ] **Step 3: Implement**

Survival definition: for each genuine probe, a full-scan match is an enrolled finger of the correct subject scoring at or above `kVerifyThreshold` with two fingers, or `kIdentifyThreshold` with one. It survives when the signature rank is within `max(1, ceil((1 - filter_rate) * gallery_size))`. The gallery holds all real non-self impressions plus `distractors` from `synthesize_template`, seeded by `seed`. Stage 1 and Stage 2 rates are timed on one thread. Use a thread pool across probes, and run the full-scan Stage 2 only once per probe per variant.

Two-finger variants treat a person as found when either finger's Stage 1 list contains that person, matching spec section 5, and then apply `apply_policy`.

Files in a variant's input with a stored `.ansi` or `.iso` from a vendor use `Path::VendorTemplate`.

- [ ] **Step 4: Run tests**

Run: `build/tests/fpx_tests -tc="*impressions*,*corrupt*,*ServerSide*,*Vendor*,*mixed*,*cascade*,*DPI*,*labelled*"`
Expected: PASS. If mismatched-format scores differ by more than 1%, report it, as it would be a real finding, instead of relaxing the test.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt include/fpx/lab/variant.hpp src/lab tests
git commit -m "feat: add single-variant lab runner"
```

### Task 7: Matrix runner and reports

**Files:**
- Create: `tests/variants/matrix.yaml`, `tools/lab/requirements.txt`, `tools/lab/run_matrix.py`, `tools/lab/report.py`, `tests/lab/test_run_matrix.py`
- Modify: `.gitignore` (add `lab-results/`), `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: `fpx-lab run` (Task 6), `fpx mockgen` (Task 3).
- Produces: `run_matrix.py --fpx-lab BIN --manifest M --tier smoke|pairwise|full --out DIR [--jobs N] [--distractors N]`. Writes `DIR/<variant_id>.json`, where `variant_id` is a stable hash of the variant's values. `report.py --in DIR --out report.md --json summary.json` writes the comparison, grouped by each dimension, with evidence labels and unsupported FMR points flagged. Importable: `expand(matrix, tier, has_scanners) -> list[dict]`.

- [ ] **Step 1: Write the failing tests**

```python
def test_full_matrix_counts(matrix):
    assert len(expand(matrix, "full", has_scanners=True)) == 3360
    assert len(expand(matrix, "full", has_scanners=False)) == 840

def test_smoke_has_one_variant_per_path(matrix):
    assert sorted(v["path"] for v in expand(matrix, "smoke", False)) == sorted(matrix["paths"])

def test_pairwise_covers_every_pair_and_is_smaller(matrix):
    variants = expand(matrix, "pairwise", True)
    assert covers_all_value_pairs(variants, matrix) and len(variants) < 3360

def test_variant_ids_are_stable(matrix):
    assert expand(matrix, "full", True) == expand(matrix, "full", True)

def test_interrupted_run_resumes(fake_lab, tmp_path):
    run_with_crash_after(fake_lab, tmp_path, n=2)
    run_again(fake_lab, tmp_path)
    assert fake_lab.calls_for_finished == 1
    assert no_partial_files(tmp_path)

def test_missing_scanner_data_is_recorded_as_skipped(real_lab, mock_manifest, tmp_path):
    run_tier(real_lab, mock_manifest, "smoke", tmp_path, scanners=["digitalpersona"])
    assert any(r["status"] == "skipped" and r["skip_reason"] for r in results(tmp_path))

def test_report_flags_unsupported_fmr_and_labels_evidence(results_dir):
    text = report(results_dir)
    assert "not supported by sample size" in text and "synthetic" in text
```

- [ ] **Step 2: Run to verify they fail**

Run: `pip install -r tools/lab/requirements.txt && FPX_LAB_BIN=build/fpx-lab pytest tests/lab/test_run_matrix.py -v`
Expected: FAIL with import errors.

- [ ] **Step 3: Implement**

`matrix.yaml` lists the six dimensions and values from spec section 16, exactly. Write each result to a temporary file and rename it, so that a crash leaves no partial result. Skip variants whose final file exists. Use `allpairspy` for pairwise with a fixed order, so that results stay reproducible.

CI adds a `lab-smoke` job: build, `fpx mockgen` a small corpus, run the smoke tier, generate the report, and upload it as an artifact. Must finish within the PR budget of about 15 minutes.

- [ ] **Step 4: Run tests and the smoke tier**

Run: `FPX_LAB_BIN=build/fpx-lab pytest tests/lab/test_run_matrix.py -v`
Expected: PASS.
Run the smoke tier on a `fpx mockgen` corpus, and generate the report.
Expected: 5 result files, with `VendorTemplate` skipped with a reason.

- [ ] **Step 5: Commit**

```bash
git add tests/variants tests/lab tools/lab .gitignore .github/workflows/ci.yml
git commit -m "feat: add variant matrix runner and reports"
```

### Task 8: Synthetic signature sweep

**Files:**
- Create: `tools/lab/sweep_signature.py`, `docs/reports/milestone-2-synthetic-sweep.md`
- Modify: `include/fpx/signature.hpp` (defaults only, if the sweep justifies it)

**Interfaces:**
- Consumes: `fpx-lab run`, with overrides `--signature "bytes=64,d_shift=3,..."`; add this flag here.
- Produces: `SignatureConfig` defaults used by Task 9.

- [ ] **Step 1: Write the check for the sweep output**

Add `test_sweep_summary_shape` to `tests/lab/test_run_matrix.py`: each row has `config`, `survival_99`, `survival_99_9`, `stage1_per_core_per_s` and `evidence == "synthetic"`.

- [ ] **Step 2: Run to verify it fails**

Run: `FPX_LAB_BIN=build/fpx-lab pytest tests/lab/test_run_matrix.py -k sweep -v`
Expected: FAIL, script missing.

- [ ] **Step 3: Implement and run the sweep**

Sweep `bytes` in {32, 64, 128}, `d_shift` in {2, 3, 4}, `spread` in {0, 1}, `target_bits_pct` in {15, 25, 35}, with 1M synthetic distractors at the full-pipeline image level for probes. Record Stage 1 rate, survival at both filter rates, and memory.

- [ ] **Step 4: Write up, and set defaults**

The report states that synthetic results choose defaults for testing and cannot pass the gate. Update the core-count estimates from spec section 3 with measured Stage 1 and Stage 2 rates, labelled `projected`.

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass with the new defaults.

- [ ] **Step 5: Commit**

```bash
git add tools/lab tests/lab docs/reports include/fpx/signature.hpp
git commit -m "feat: sweep signature configs on synthetic data"
```

### Task 9: Gate and WSQ reports

This task needs owner input. Public datasets are required for a gate verdict, and scans from both scanners are required for cross-device results.

**Files:**
- Create: `tools/lab/gate.py`, `docs/reports/milestone-2-gate.md`, `docs/reports/milestone-2-wsq-impact.md`
- Modify: `docs/ROADMAP.md`, `docs/SCALING.md`, `docs/MATCHING.md`

**Interfaces:**
- Consumes: results from Task 7, `gate_verdict` through `fpx-lab gate --in DIR --out gate.json`; add this subcommand here.
- Produces: `gate.json` with `result`, `provisional`, `reason`, and real and synthetic counts. Milestone 3's plan starts from this.

- [ ] **Step 1: Write the failing test**

```python
def test_gate_without_real_data_says_not_evaluated(synthetic_results, tmp_path):
    assert gate(synthetic_results, tmp_path)["result"] == "NotEvaluated"

def test_wsq_comparison_pairs_the_same_fingers(results_dir):
    rows = wsq_impact(results_dir)
    assert {r["path"] for r in rows} == {"BmpDirect", "Wsq075", "Wsq225"}
    assert all(r["same_corpus"] for r in rows)
```

- [ ] **Step 2: Run to verify it fails**

Run: `FPX_LAB_BIN=build/fpx-lab pytest tests/lab/test_run_matrix.py -k "gate or wsq" -v`
Expected: FAIL.

- [ ] **Step 3: Implement `gate.py` and the `gate` subcommand**

- [ ] **Step 4: Run on real data**

Run the pairwise tier and full-scan and cascade variants at 1M distractors, on FVC-style public data stored outside the repository, using a manifest. Peak RAM must fit, or run on a larger machine. If scans from both scanners exist, also run the full tier.

If no real data has been supplied, write the reports with `NotEvaluated`, stop, and tell the owner what is missing. Do not substitute synthetic results.

- [ ] **Step 5: Write the reports and present the decision**

`milestone-2-gate.md`: verdict, real and synthetic counts, Stage 1 and Stage 2 rates, updated cores and RAM labelled `projected`, and the next step from the spec's gate table.

`milestone-2-wsq-impact.md`: FNMR at each FMR point for BMP direct, WSQ 0.75 and WSQ 2.25, and average file sizes per scanner, replacing the spec's size estimate. Recommend, but do not set, a conversion order. The owner decides.

Stop here for the owner's decision. Do not begin milestone 3.

- [ ] **Step 6: Commit**

```bash
git add tools/lab tests/lab src/lab docs
git commit -m "docs: report milestone 2 gate and WSQ results"
```
