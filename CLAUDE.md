# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A fingerprint toolkit and 1:N identification server in C++17: scanner bitmap → WSQ → ANSI-378 ("MINEX") / ISO-19794-2 template → 1:1 match / 1:N search. FingerJet FX OSE does extraction, but it has no matcher, so the minutiae matcher (`src/core/matcher.cpp`) is this repo's own code. All tests, benchmarks and demos use **synthetic fingerprints** (`src/core/synthetic.cpp`). Real scans and public datasets never go into git or CI.

## Build and test

`scripts/run-all.sh` runs every stage below (build, ctest, sanitizers with `--full`, accuracy, benchmarks, all examples, Go gateway, HTTP load test, web + Playwright, Docker, deploy checks), with `--quick`, `--only a,b` and `--skip a,b`. Logs go to `test-reports/<timestamp>/`.

```bash
cmake -S . -B build -Wno-dev                 # Release by default; first configure fetches pinned deps (network)
cmake --build build -j
(cd build && ctest --output-on-failure)      # doctest suite + CLI smoke tests + Python REST e2e
```

- Single doctest case or pattern: `build/tests/fpx_tests -tc="*WSQ*"`. List cases: `build/tests/fpx_tests -ltc`.
- Single ctest target: `cd build && ctest -R server_e2e --output-on-failure` (others: `fpx_tests`, `cli_demo`, `cli_bench_small`, `server_demo_e2e`).
- The REST e2e tests (`tests/server_test.py`, `tests/server_demo_test.py`) use only the Python stdlib. They are registered only if Python 3 is found and `FPX_BUILD_SERVER=ON`.
- CMake options: `FPX_BUILD_TESTS`, `FPX_BUILD_SERVER`, `FPX_BUILD_EXAMPLES` (all ON), `FPX_NATIVE_ARCH` (OFF). Offline builds: `-DFPX_FINGERJET_SOURCE_DIR=/path/to/FingerJetFXOSE` (see `cmake/Dependencies.cmake`).
- On this machine the checkout is on a root-owned NTFS mount: git (and CMake FetchContent, `go build`) fail with "dubious ownership". Use `GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0='*'` for the process; `run-all.sh` sets it. CMake came from `uv tool install cmake` (no sudo).
- HTTP load test: `build/fpx gendb --db F --subjects N --fingers 2 --probes 40 --probe-dir D`, start `fpx-server --db F`, then `python3 tests/load_test.py --url ... --probes D`. Gateway e2e: `tests/gateway_e2e.py`.
- New test files must be added to the `fpx_tests` source list in `tests/CMakeLists.txt`. Tests write temp files under the `FPX_TEST_TMP` compile definition.

Release CI (`.github/workflows/ci.yml`) also runs accuracy and speed checks you can run locally:

```bash
build/fpx eval --synthetic 20x3
build/fpx bench --gallery 100000 --probes 10
build/fpx bench --gallery 50000 --probes 10 --fingers 2
```

Other CI jobs: `go vet`/`go build` in `examples/go-gateway`, `shellcheck deploy/remote-deploy.sh`, a Docker build (it runs the test suite inside the build stage), and the web app checks below.

### Web demo (`apps/web`, React + Vite + TS)

```bash
./scripts/demo.sh                          # builds fpx-server + web, serves http://127.0.0.1:8080 (--demo: no auth)
cd apps/web && npm ci && npm test          # vitest (src/**/*.test.ts)
npm run build                              # tsc --noEmit && vite build
npm run dev                                # Vite dev server; proxies /v1 and /healthz to 127.0.0.1:8080
FPX_SERVER=../../build/fpx-server npm run e2e   # Playwright; needs `npm run build` first
```

Screenshot baselines are per OS: `e2e/snapshots/<name>-<platform>.png` (font rendering differs between macOS and Linux by more than the 2 % tolerance). After an intended UI change, refresh with `npm run e2e -- --update-snapshots=all`. Plain `--update-snapshots` only rewrites failing ones, so a change inside the tolerance is kept stale. `DOCS_ASSETS=../../docs/assets npm run e2e` also refreshes the README screenshots.

## Architecture

The build produces these targets from one static core library, `fpx_core` (`src/core/*`, public headers in `include/fpx/`):
- `fpx` (CLI, `src/cli/main.cpp`): info / convert / extract / match / enroll / identify / eval / bench / demo
- `fpx-server` (`src/server/main.cpp`): REST API on cpp-httplib + nlohmann/json, optional `X-API-Key` auth (`FPX_API_KEY` or `--api-key`)
- `libfpx.so` (`src/core/c_api.cpp`, `include/fpx/fpx_c.h`): stable C ABI for FFI (Python ctypes, C#, Java, Go). ABI changes affect `tests/test_c_api.cpp` and `examples/python/fpx_ctypes.py`.
- `libFRFXLL.so`: FingerJet, built as a **separate shared library** on purpose so the LGPL-3.0 component stays replaceable. Do not link it statically.

Core pipeline modules:
- **image**: decodes BMP/PGM/TIFF/WSQ into `GrayImage`. (The v1 bug was passing the raw BMP file bytes to FingerJet as pixels.) DPI comes from headers when plausible. A wrong DPI is the most common cause of bad matching.
- **extractor**: FingerJet → ANSI/ISO. It crops around the detected finger to get within FingerJet's analysis-window limit, then maps the coordinates back.
- **template**: parses and serialises ANSI-378-2004 / ISO-19794-2-2005 from any vendor, auto-detects the format and converts between the two.
- **matcher**: score 0–100. The algorithm and calibration are in `docs/MATCHING.md`. Default thresholds are `kVerifyThreshold` = 12 and `kIdentifyThreshold` = 18. Thresholds are validated on synthetic data only.
- **gallery**: in-RAM 1:N index. Templates are compiled into packed arenas and scanned linearly by a thread pool, with a top-k heap per thread that is merged at the end. A *subject* has up to 10 finger slots (ISO positions 1–10, 0 = unknown). Each probe finger is compared only with enrolled fingers of the same position (or with all of them when either side is 0). Per-subject scores are combined by a `FusionPolicy`: every presented finger (≥ 2) scores ≥ finger threshold, **or** one finger scores ≥ the strong-single threshold. A single-finger probe uses the stricter identify threshold. Searches share one thread pool and are admitted FIFO (`ThreadPool::run`); exceptions from any worker are rethrown to the caller.
- **store** (`*.fpxdb`): durable append-only log with CRC, torn-write recovery and compaction. On startup the server replays it into the gallery. Key format: `id` for finger 0 (v2.0 compatible) and `id\x1F<pos>` for positioned fingers. Keep this format so that existing stores still load.

Compatibility: v2.0 single-template REST requests (`{"template": ...}` without `fingers`) and v2.0 stores must keep working.

Server endpoints and the two-finger decision flags are listed in `README.md`. When sharding, all fingers of one subject must live on the same node (see `examples/go-gateway`).

## Docs and planned work

- `docs/ARCHITECTURE.md`, `MATCHING.md`, `SCALING.md`, `CAPACITY.md`, `TESTING.md`, `DEPLOYMENT.md`
- The scale-platform design (gateways, virtual shards, bit-signature pre-filter, mobile apps) is in `docs/superpowers/specs/` and `docs/superpowers/plans/`, with status in `docs/ROADMAP.md`. Most of it is **planned, not built**. Docs must describe what exists today as current and must not present planned features as working.
- `apps/web/src/benchmarks.json` and `docs/CAPACITY.md` hold measured numbers. Every reported number is labelled `measured`, `synthetic` or `projected`.

## Deployment

Every push to `master` runs `.github/workflows/deploy.yml`: CI as a gate → Docker image published to `ghcr.io/rabbicse/fp-extractor` → SSH deploy via `deploy/remote-deploy.sh` and `deploy/docker-compose.prod.yml`, with a health check and automatic rollback. Other branches and PRs run only `ci.yml`. Local Docker setup: `docker/Dockerfile`, plus `docker/docker-compose.yml` (the `sharded` profile starts 2 shards and the Go gateway on :9090).
