# TODO / resume notes

Last session: 2026-11-04 (evening) to 2026-11-05 (midnight). All work is merged
into local `master`, which is ahead of `origin/master` and not pushed yet: a push to
`master` runs the auto-deploy (tests -> Docker image -> SSH deploy).

## Resume

```bash
cd /mnt/7A4CEE3F674E3964/github/fp-extractor
git checkout master
scripts/run-all.sh --quick          # ~2 min sanity check, everything should pass
```

This checkout is on a root-owned NTFS mount: plain `git` fails with "dubious
ownership". Use `git -c safe.directory='*' ...` or export
`GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0='*'`.
CMake and shellcheck were installed with `uv tool install` (no sudo).

## Done (committed on the branch)

- Concurrency: FIFO search queue, 64 HTTP threads (16 clients: p95 32 s -> 13.5 s).
- Thread-pool exceptions rethrown instead of crashing the server.
- Perf: 1:N search 1.8x faster with bit-identical scores (1M templates 3.95 s -> 2.22 s,
  1M x 2 fingers 7.98 s -> 5.44 s, HTTP +33 %). Exact score bound skips alignment,
  branchless hot loops, radix sort.
- Determinism: tied scores gave different top-k between runs; now a total order.
- `-ffp-contract=off`: scores were different on ARM64 / `-march=native` builds.
- FingerJet built with `-fwrapv` (UBSan: signed overflow in its fixed-point code).
- Web: per-OS screenshot baselines, legend for "other" minutiae.
- `scripts/run-all.sh`, `tests/load_test.py`, `tests/gateway_e2e.py`, `fpx gendb`.
- Docker test image: `docker/Dockerfile.test`, `scripts/test-in-docker.sh`.
- Examples: Go via cgo (`examples/go-cgo`), C# .NET 10 via P/Invoke (`examples/csharp`),
  both verified (score 77.1 same finger, 0.2 different finger, same as Python).

## Next, in order

1. **Verify the Docker test image**: `scripts/test-in-docker.sh --quick --skip docker`.
   Last run (before the final two fixes): 9 stages PASS inside the image (build, unit, accuracy,
   bench, examples, gateway, load); `web` failed (Playwright folder not writable by the
   non-root user) and `deploy` failed (older shellcheck code SC2317). Both are fixed but not
   yet re-verified. The `web` stage may then show screenshot differences, because the image
   renders fonts differently from the host: if so, make the image the reference (below).
   Then consider `FROM ubuntu:26.04` for the test image (host is Ubuntu 26.04) and
   regenerate Playwright baselines from the image (`scripts/test-in-docker.sh --update-snapshots`)
   so screenshots are identical on every machine.
2. **README rewrite** (asked for): how to test (run-all, test-in-docker, single tests), full
   end-to-end flow, language integration with code for C++, Python (ctypes + REST), Go (cgo +
   gateway), C# (.NET 10 P/Invoke + REST with HttpClient), and a benchmark section with the
   numbers below (before/after table, hardware, how to reproduce).
3. **Technical design document with diagrams** (asked for): full system design and
   end-to-end flow for visualisation - components, data flow, matcher pipeline stages with
   their measured cost, concurrency model (HTTP threads -> FIFO pool -> chunked scan ->
   per-thread top-k), storage and replay, sharding via the Go gateway, deployment.
   Publish as an artifact page and keep `docs/ARCHITECTURE.md` in sync.
4. **User samples in `samle/`** (gitignored, real biometric data - never commit):
   - `fp_1.bmp`: rejected as "unrecognised image format" although it is a .bmp (157 KB,
     other BMPs are 154 KB) - check the header (BMP V4/V5? compression?).
   - `test_out.wsq`: rejected as unrecognised - check the WSQ markers.
   - `f1.bin`, `fid_0.bin`, `fid_1.bin`: ISO/IEC 19794-4 finger image records
     (`FIR\0010` header) - add a decoder (header + raw/WSQ image data).
   - Templates `fin.minex` (ANSI), `minex.dat`, `minex_0.bin`, `minex_1.bin` (ISO) load fine:
     try `fpx match` across them and `fpx eval` once there are pairs of the same finger.
   - Real-data accuracy: thresholds are calibrated on synthetic data only.
5. Go load generator for the scale targets (open-loop, constant arrival rate, thousands of
   connections, latency histogram); keep `tests/load_test.py` as the correctness check.
6. CI: add the Go cgo example and the C# example (dotnet 10 container) to `ci.yml`;
   optionally run the test image in CI.
7. More speed (exact): stage 1 edge look-up is now the biggest fixed cost (~5 us of ~17 us
   per comparison, 671 index items scanned per comparison); multi-finger search prunes only
   below 3.0 - a per-subject top-k bound would prune more. Larger wins need the planned
   bit-signature pre-filter (roadmap stage 2), which is a go/no-go decision, not exact.
8. `git push origin master` when ready to deploy (CI runs all tests first and rolls back on a
   failed health check). The deploy needs the GitHub settings in docs/DEPLOYMENT.md.

## Measured (i7-6700, 4C/8T, 30 GB, Ubuntu 26.04, GCC 15, synthetic data)

| Benchmark | Before | After |
|---|---|---|
| 1M templates, 1 finger | 3,952 ms | 2,221 ms |
| 100k templates, 1 finger | 370 ms | 249 ms |
| 1M customers x 2 fingers | 7,979 ms | 5,438 ms |
| 100k customers x 2 fingers | 854 ms | 628 ms |
| HTTP, 100k x 2 fingers, 1 client | 1.18 searches/s, p50 874 ms | 1.65 searches/s, p50 618 ms |
| HTTP, 16 clients | 1.27 searches/s, p95 13.5 s | 1.69 searches/s, p95 10.2 s |

Accuracy unchanged: synthetic EER 0 %, no false matches; per-comparison scores bit-identical
(hash 1856318975ae00ef over 480k comparisons in the profiling harness).
