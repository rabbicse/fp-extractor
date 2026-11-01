# Architecture

## Goal

Any scanner, any vendor: the device turns a fingerprint bitmap into a standard
minutiae template (ANSI-378 "MINEX" or ISO-19794-2) and sends **only that**
(0.3–1.5 KB) to the server. The server stores templates and answers
"who is this?" against ~1 M customers in seconds.

```
 DEVICE SIDE (any scanner SDK)                       SERVER SIDE
┌────────────────────────────────────┐      ┌──────────────────────────────────────────────┐
│ scanner SDK → BMP / raw pixels     │      │  API (fpx-server, or Go gateway + shards)    │
│        │                           │      │    PUT  /v1/subjects/{id}   enrol            │
│        ▼                           │      │    POST /v1/identify        1:N              │
│ libfpx (C/C++/Python/C#/Java/Go)   │ HTTPS│    POST /v1/verify          1:1              │
│  • decode BMP/PGM/TIFF/raw         │─────▶│        │                                     │
│  • WSQ encode (archive, optional)  │ JSON │        ▼                                     │
│  • FingerJet → ANSI-378 / ISO      │ b64  │  Gallery (RAM, compiled templates, arenas)   │
│                                    │      │   • parallel scan over all cores             │
└────────────────────────────────────┘      │   • top-k heap per thread, merge             │
                                            │        │ append-only log (durability)        │
                                            │        ▼                                     │
                                            │  TemplateStore (*.fpxdb)  ← or PostgreSQL     │
                                            └──────────────────────────────────────────────┘
```

Thin clients that cannot run native code may send the image instead
(`"image": base64`); the server then runs the same extraction. Sending
templates is still recommended: less bandwidth, no raw biometric images in
transit, and server CPU is reserved for matching.

## Components

| Module | Files | Responsibility |
|---|---|---|
| Image I/O | `include/fpx/image.hpp`, `src/core/image.cpp` | BMP (1/4/8/24/32-bit, top-down/bottom-up, padded rows, palettes), PGM, uncompressed TIFF (FVC datasets), raw buffers, WSQ via NBIS. DPI read from headers when plausible. |
| WSQ | `wsq.hpp/.cpp`, `third_party/nbis_wsq` | FBI WSQ encode/decode using the NIST NBIS reference codec (public domain, vendored subset). |
| Extraction | `extractor.hpp/.cpp` | FingerJet FX OSE (DigitalPersona, LGPL) → ANSI-378-2004 / ISO-19794-2-2005. Handles FingerJet's analysis-window limit by cropping around the detected finger and mapping coordinates back. |
| Template codec | `template.hpp/.cpp` | Parse/serialise ANSI and ISO records, auto-detect format, convert between them. Works with templates from **any** vendor. |
| Matcher | `matcher.hpp/.cpp` | Minutiae matcher (see [MATCHING.md](MATCHING.md)). Score 0–100. |
| Gallery | `gallery.hpp/.cpp` | In-memory 1:N index: packed arenas, thread pool, top-k, insert/replace/remove, compaction. |
| Store | `store.hpp/.cpp` | Durable append-only log with CRC; torn-write recovery; compaction. |
| Synthetic data | `synthetic.hpp/.cpp` | Realistic synthetic fingerprint images and templates for tests/benchmarks: no real biometrics in the repo. |
| C API | `fpx_c.h`, `c_api.cpp` → `libfpx.so` | Stable ABI for Python, C#, Java, Go, Node. |
| CLI | `src/cli/main.cpp` → `fpx` | convert / extract / match / enrol / identify / eval / bench / demo. |
| Server | `src/server/main.cpp` → `fpx-server` | REST API (cpp-httplib + nlohmann/json), API-key auth. |
| Go gateway | `examples/go-gateway` | Reference API layer that shards across many `fpx-server` nodes. |

## Why these choices

* **FingerJet only for extraction.** It is the open-source DigitalPersona
  extractor that this repository already used. It does not include a matcher
  (DigitalPersona's matcher is commercial), so a matcher had to be written.
  That's the missing "MINEX matching" piece.
* **Standard templates on the wire.** ANSI-378 / ISO-19794-2 is what every
  scanner SDK can produce, so the server never depends on a device model.
  Templates from other extractors (Neurotechnology, Innovatrics, SecuGen,
  NBIS mindtct, ...) are accepted too. Note that cross-extractor accuracy is
  always lower than same-extractor accuracy; for best results use one
  extractor (this library) everywhere.
* **Matching in RAM, persistence on disk.** A 1:N search has to touch every
  template; no database can do that faster than a linear scan over packed
  memory. The database is only for durability and administration.
* **C++ engine, any language for the API.** See "Go or C++?" below.

## Data flow

1. **Capture** (device): scanner SDK → 8-bit grayscale bitmap at the sensor's
   real DPI (usually 500).
2. **Convert** (device, `libfpx`): optional `wsq_encode` for archival;
   `extract` → ANSI or ISO bytes.
3. **Enrol**: `PUT /v1/subjects/{id}` with the template → appended to the store
   (durable) → compiled and added to the in-memory gallery.
4. **Identify**: `POST /v1/identify` → probe indexed once → all gallery
   templates scored in parallel → top-k → `match` if best ≥ identify threshold.
5. **Restart**: the store is replayed into RAM (≈ 15 s per million templates
   on 4 cores, mostly compilation).

## Go or C++ for the API?

**Both, each where it is strong:**

* **Go for the API / business layer**: authentication, customer records,
  PostgreSQL, rate limiting, audit trail, metrics, fan-out to shards. Easy to
  write, deploy and staff; none of this is CPU-bound.
* **C++ for the matching engine**: the inner loop runs ~10⁶ comparisons per
  search and needs tight memory layout, no GC pauses and SIMD-friendly code.
  It already exists here and only needs to be operated, not rewritten.
* **How they talk:** (a) recommended: Go → HTTP/gRPC → C++ matcher nodes
  (`fpx-server`), each owning a shard; process isolation, independent
  scaling (see `examples/go-gateway`). (b) Single box: Go → cgo → `libfpx.so`
  (`fpx_c.h`); simpler, but a crash in native code takes the Go process down.

## Security & privacy notes

* Templates are biometric data. Encrypt the store at rest (disk/volume
  encryption) and use TLS in front of the server (nginx/envoy/ingress).
* Set `FPX_API_KEY` (or put the server behind your gateway); never expose it
  directly to the internet.
* Raw images (WSQ) are only needed for re-extraction/audit; keep them in a
  separate, access-controlled store if you keep them at all.
* Never commit real fingerprints to the repository: tests use synthetic data.

## Licensing

* This repository's code: MIT (see SPDX headers).
* FingerJet FX OSE: **LGPL-3.0**, fetched at build time and built as a
  separate shared library (`libFRFXLL.so`) so it remains replaceable.
* NBIS WSQ subset: public domain (NIST).
* cpp-httplib, nlohmann/json, doctest: MIT.
