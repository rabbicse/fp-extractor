# fp-extractor

Fingerprint toolkit and identification server, independent of the scanner:

**scanner bitmap → WSQ → ANSI-378 ("MINEX") / ISO-19794-2 template → 1:1 match / 1:N search over millions.**

* **Extraction** with [FingerJet FX OSE](https://github.com/FingerJetFXOSE/FingerJetFXOSE) (DigitalPersona's open-source extractor)
* **WSQ** compression with the NIST NBIS reference codec
* **Matching** of ANSI-378 / ISO-19794-2 templates from any vendor (FingerJet has no matcher; this repo adds one)
* **1:N identification**: 1,000,000 templates in **~5 s on 4 cores** (~1.5 GB RAM); target was ≤ 30–60 s
* **CLI** (`fpx`), **REST server** (`fpx-server`), **C API** (`libfpx.so`) for Python / C# / Java / Go
* Tests, benchmarks and demos use **synthetic fingerprints**, so no real biometric data is needed

> Status: v2 rewrite on branch `feature/identification-engine`. The matcher is
> validated on synthetic data only; **calibrate thresholds on your own
> scanner data before production** ([docs/MATCHING.md](docs/MATCHING.md#calibration)).
> For million-scale 1:N, enrol **two fingers per customer**: single-finger
> margins at 1 M are thin ([docs/SCALING.md](docs/SCALING.md)).

---

## Quick start (Linux / macOS / WSL)

Requirements: C++17 compiler (GCC ≥ 9 or Clang ≥ 10), CMake ≥ 3.16, git, network
access on the first build (dependencies are fetched and pinned).

```bash
git clone -b feature/identification-engine https://github.com/rabbicse/fp-extractor
cd fp-extractor
cmake -S . -B build -Wno-dev            # Release by default
cmake --build build -j
(cd build && ctest --output-on-failure)  # 27 test cases + CLI smoke tests
```

Ubuntu packages: `sudo apt install build-essential cmake git`.

### 1. See it work: no scanner needed

```bash
build/fpx demo --out fpx-demo
```

Generates 3 synthetic fingers × 2 impressions, then runs each through
BMP → WSQ → ISO/ANSI and prints a score matrix and 1:N results:

```
== 3. Score matrix (genuine pairs on the diagonal blocks)
          f1_0  f1_1  f2_0  f2_1  f3_0  f3_1
   f1_0   100.0   79.8    0.1    0.5    0.4    0.1
   f1_1    79.8  100.0    0.0    0.0    0.1    0.3
   f2_0     0.1    0.0  100.0   72.4    0.1    0.4
   ...
== 4. 1:N: enrol impression 0 of each finger, identify impression 1
   probe finger1_1 -> finger1 (score 79.8)
```

### 2. Your scanner's bitmap → WSQ → MINEX template

```bash
build/fpx info    scan.bmp                              # size / dpi / format
build/fpx convert scan.bmp scan.wsq                     # WSQ (≈15:1)
build/fpx extract scan.bmp -o scan.ansi --format ansi   # ANSI-378 = MINEX format
build/fpx extract scan.bmp -o scan.iso  --format iso    # ISO-19794-2
build/fpx info    scan.ansi -v                          # list minutiae
```

Inputs: BMP (1/4/8/24/32-bit), PGM, uncompressed TIFF, WSQ. Use `--dpi N` if
the file doesn't carry the sensor's real resolution (most are 500 dpi).

### 3. Match (this was the missing piece)

```bash
build/fpx match scan.ansi other.bmp -v     # templates and/or images
# score:     79.80
# threshold: 12.00
# decision:  MATCH
```

### 4. 1:N identification

```bash
build/fpx enroll   --db customers.fpxdb --id customer-001 scan.ansi
build/fpx identify --db customers.fpxdb probe.bmp --top 5
build/fpx bench --gallery 1000000          # 1 M synthetic customers: speed + accuracy
```

### 5. Server

```bash
FPX_API_KEY=change-me build/fpx-server --db customers.fpxdb --port 8080
```

| Method | Path | Body (JSON, binary fields base64) | Result |
|---|---|---|---|
| GET | `/healthz` | | `{"status":"ok"}` |
| GET | `/v1/stats` | | template count, memory, thresholds |
| POST | `/v1/extract` | `{"image", "dpi"?, "format"?: "iso"\|"ansi", "wsq"?: true}` | `{"template", "minutiae", "wsq"?}` |
| PUT | `/v1/subjects/{id}` | `{"template"}` or `{"image"}` | enrol / replace |
| DELETE | `/v1/subjects/{id}` | | remove |
| POST | `/v1/identify` | `{"template"\|"image", "top_k"?: 5}` | `{"match", "id", "candidates":[{id,score,match}], "elapsed_ms"}` |
| POST | `/v1/verify` | `{"id", "template"\|"image"}` | `{"score", "match"}` |
| POST | `/v1/match` | `{"a", "b"}` (two templates) | `{"score", "match"}` |

All requests need `X-API-Key` when a key is set. Clients:
`examples/python/client.py`, `examples/http/requests.http`.

**Device-agnostic by design:** the device converts the bitmap to a template
(with `libfpx`, or any vendor SDK that outputs ANSI-378/ISO-19794-2) and
sends only that ~0.5 KB. Thin clients may send the image instead.

### 6. Docker

```bash
docker build -f docker/Dockerfile -t fp-extractor .
docker run -p 8080:8080 -e FPX_API_KEY=change-me -v fpx-data:/data fp-extractor
# sharded: 2 matcher shards + Go gateway on :9090
docker compose -f docker/docker-compose.yml --profile sharded up --build
```

---

## Using it from code

**C++**

```cpp
#include "fpx/fpx.hpp"
fpx::GrayImage img = fpx::load_image("scan.bmp");             // BMP/PGM/TIFF/WSQ
std::vector<uint8_t> wsq = fpx::wsq_encode(img);               // archive
fpx::ExtractOptions o; o.format = fpx::TemplateFormat::Ansi378_2004;
fpx::ExtractResult t = fpx::extract(img, o);                   // t.record = MINEX bytes
fpx::Gallery g;  g.insert("customer-1", t.parsed);
auto r = g.identify(fpx::extract(fpx::load_image("probe.bmp")).parsed, {5});
```

Full example: `examples/cpp/pipeline.cpp`.

**Python / C# / Java / Go**: use the C API in `include/fpx/fpx_c.h`
(`libfpx.so`). Python example: `examples/python/fpx_ctypes.py`.

---

## Documentation

| Doc | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | components, data flow, Go vs C++, security, licensing |
| [docs/MATCHING.md](docs/MATCHING.md) | the matching algorithm, calibration, improving accuracy |
| [docs/SCALING.md](docs/SCALING.md) | 1 M+ identification: benchmarks, RAM, database choice, sharding |
| [docs/TESTING.md](docs/TESTING.md) | step-by-step testing: unit tests, manual walk-through, accuracy eval, benchmarks |

## Repository layout

```
include/fpx/        public C++ API + C API (fpx_c.h)
src/core/           image I/O, WSQ, extractor, template codec, matcher, gallery, store, synthetic
src/cli/            fpx command-line tool
src/server/         fpx-server REST API
tests/              doctest unit + integration tests
examples/           C++, Python (ctypes + REST), HTTP requests, Go sharding gateway
third_party/        NBIS WSQ subset (public domain)
cmake/              dependency pinning (FingerJet, httplib, json, doctest)
docker/             Dockerfile, docker-compose (single + sharded)
docs/               architecture, matching, scaling, testing
```

## What changed from v1

* **Bug fixed:** v1 passed the whole BMP *file* (header, palette, bottom-up
  padded rows) to FingerJet as if it were raw pixels, so the templates were
  extracted from a garbled image. v2 decodes the BMP properly.
* **Bug fixed:** v1's "match score" was `memcmp` of two templates; it only
  tells you whether the bytes are identical. v2 has a real minutiae matcher.
* Hard-coded paths and prebuilt `.so` files are replaced by a reproducible
  CMake build; generated build files (CMakeCache, Makefile, .idea) were removed.

## Licenses

MIT for this repository's code. FingerJet FX OSE is **LGPL-3.0** (built as a
separate shared library `libFRFXLL.so`); NBIS is public domain; cpp-httplib,
nlohmann/json and doctest are MIT. Not MINEX-certified: NIST certification is
a separate formal process.
