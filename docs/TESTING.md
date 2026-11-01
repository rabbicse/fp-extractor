# Testing guide

Everything here runs without a scanner and without real fingerprints, because
the repository includes a synthetic fingerprint generator. Real-data steps
are at the end.

## 1. Automated tests

```bash
cmake -S . -B build -Wno-dev
cmake --build build -j
cd build && ctest --output-on-failure
```

| Test | What it proves |
|---|---|
| `test_image.cpp` | BMP (8/24-bit, top-down/bottom-up, odd widths), PGM, malformed input rejection, **the original bug** (BMP header bytes fed to FingerJet as pixels), WSQ round trip & compression ratio |
| `test_template.cpp` | ANSI-378 / ISO-19794-2 serialise ↔ parse, record sizes per the standards, format auto-detection, ANSI↔ISO conversion |
| `test_matcher.cpp` | self-match = 100, symmetry, rotation/translation recovery, resolution normalisation (1000 dpi vs 500 dpi), genuine vs impostor separation, degenerate input |
| `test_gallery_store.cpp` | correct identification among 3,000 templates, replace/remove/compact, same results for 1 and 4 threads, store replay, torn-write recovery, compaction |
| `test_pipeline.cpp` | **real FingerJet** end to end: synthetic scan → BMP → WSQ → ISO/ANSI → match; off-centre finger on a large platen; blank image error |
| `test_c_api.cpp` | the C ABI used by Python/C#/Java/Go |
| `test_multifinger.cpp` | fusion policy rules, two-finger 1:N among 1,500 customers, strangers rejected, position filtering (index never compared with thumb slot), unknown positions, per-finger add/replace/remove, two-finger verify, store key compatibility |
| `server_e2e` (`server_test.py`) | starts the real `fpx-server`: auth, two-finger enrol (template + image), GET subject, two-finger and legacy identify, stranger rejected, verify, per-finger routes, validation errors, online compaction, **restart persistence** |
| `cli_demo`, `cli_bench_small` | the CLI works end to end |

Run a subset: `build/tests/fpx_tests -tc="*WSQ*"`; list: `build/tests/fpx_tests -ltc`.

## 2. Manual walk-through (5 minutes)

```bash
# a) end-to-end demo: writes BMP, WSQ, ISO, ANSI files and prints a score matrix
build/fpx demo --out fpx-demo

# b) your own scanner bitmap -> WSQ -> template
build/fpx info   my_scan.bmp                       # size, dpi, format
build/fpx convert my_scan.bmp my_scan.wsq          # WSQ for archival
build/fpx extract my_scan.bmp -o my_scan.ansi --format ansi     # MINEX / ANSI-378
build/fpx extract my_scan.bmp -o my_scan.iso  --format iso      # ISO-19794-2
build/fpx info my_scan.ansi -v                     # list the minutiae

# c) 1:1 matching (templates or images, any mix)
build/fpx match my_scan.ansi second_scan.bmp -v

# d) 1:N with a persistent gallery, two fingers per person (2 = right index, 7 = left index)
build/fpx enroll   --db test.fpxdb --id alice alice_ri.bmp alice_li.bmp --fingers 2,7
build/fpx enroll   --db test.fpxdb --id bob   bob_ri.ansi  bob_li.ansi  --fingers 2,7
build/fpx identify --db test.fpxdb probe_ri.bmp probe_li.bmp --fingers 2,7
build/fpx identify --db test.fpxdb probe_ri.bmp --fingers 2          # one finger: stricter rule
```

If your scanner is not 500 dpi and the BMP header doesn't say so, pass
`--dpi <real dpi>`; wrong DPI is the most common cause of bad matching.

## 3. Accuracy evaluation

```bash
# synthetic: 50 fingers x 4 impressions, through the full FingerJet pipeline
build/fpx eval --synthetic 50x4

# real data: a folder of <finger>_<impression>.<bmp|tif|pgm|wsq|iso|ansi>
# e.g. FVC2002 DB1_A (100 fingers x 8): http://bias.csr.unibo.it/fvc2002/
build/fpx eval /data/FVC2002/DB1_A -v
```

Output: EER, FNMR at FMR 1 % and 0.1 %, mean/max impostor score, the five
hardest genuine and impostor pairs (with `-v`). Use it to choose thresholds
(see [MATCHING.md](MATCHING.md#calibration)) and to compare matcher settings:

```bash
build/fpx eval /data/mine -v --matcher "pair_dist_tol=18,min_pair_support=2"
```

## 4. Performance

```bash
build/fpx bench --gallery 100000  --probes 20          # quick
build/fpx bench --gallery 1000000 --probes 20          # the 1 M target (~1.5 GB RAM)
build/fpx bench --gallery 1000000 --threads 8 -v       # per-probe details
build/fpx bench --gallery 1000000 --fingers 2          # 1 M customers x 2 fingers (~3 GB RAM)
```

The bench builds a synthetic gallery, searches perturbed versions of enrolled
fingers (must be found at rank 1 above the identify threshold) and
non-enrolled fingers (best score must stay below it), and prints latency,
throughput and the projection for 1 M templates.

## 5. Server

```bash
FPX_API_KEY=secret build/fpx-server --db server.fpxdb --port 8080 &
export FPX_URL=http://localhost:8080 FPX_API_KEY=secret
python3 examples/python/client.py enroll   alice fpx-demo/finger1_0.iso:2 fpx-demo/finger2_0.bmp:7
python3 examples/python/client.py identify fpx-demo/finger1_1.bmp:2 fpx-demo/finger2_1.iso:7
python3 examples/python/client.py verify   alice fpx-demo/finger3_1.iso:2   # -> match: false
python3 examples/python/client.py get      alice
python3 examples/python/client.py stats
```

Or open `examples/http/requests.http` in VS Code (REST Client) / JetBrains.

## 6. Language bindings

```bash
python3 examples/python/fpx_ctypes.py build/libfpx.so fpx-demo/finger1_0.bmp fpx-demo/finger1_1.bmp
build/example_pipeline                       # C++ API walk-through (synthetic input)
build/example_pipeline scan1.bmp scan2.bmp   # with your own images
```
