# fp-extractor scale platform: design

Status: awaiting review. Date: 2026-11-02.

## 1. Goal

Turn fp-extractor from a single-node library and reference server into a
documented, resilient fingerprint platform with web and mobile clients.

Agreed requirements:

| Area | Requirement |
|---|---|
| Throughput | Design for 1,000 1:N searches/s |
| Gallery | 10M enrolled templates |
| Connections | 1M concurrent connections across the gateway tier |
| Web | React (Vite + TypeScript), demoable from capture through match, no scanner needed |
| Mobile | React Native for Android and iOS; live USB scanner capture on Android |
| Client flows | Two options: on-device conversion, or server-side conversion |
| Evidence | Unit, integration, visual, end-to-end, load tests and simulation |
| Documentation | Architecture diagrams and a step-by-step guide |

### Non-goals

Multi-region deployment, multi-tenancy, encryption-key management, user
accounts, an admin console. The docs list the production-relevant ones as
prerequisites. TLS and API keys are in scope.

## 2. Current state (measured 2026-11-02)

All 4 existing test suites pass on an Apple M1 (8 threads). Benchmarks:

* 200k templates, one finger: 1,274 ms average search, about 160k comparisons/s,
  so about 20k per core and about 62 s for a 10M full scan.
* 100k subjects × 2 fingers: 1,322 ms average search.

Problems this design fixes:

| Problem | Location |
|---|---|
| One search at a time per process | `src/core/gallery.cpp:35` |
| 8 HTTP worker threads by default | `src/server/main.cpp:85` |
| Writes are flushed but not fsynced | `src/core/store.cpp:165` |
| Whole store read into RAM on restart | `src/core/store.cpp:124` |
| Failed shard still returns HTTP 200 with `partial` | `examples/go-gateway/main.go:201` |
| No replicas, `hash % N` sharding, no admission control | `examples/go-gateway/main.go` |
| Accuracy tested only on synthetic data | `docs/MATCHING.md` |

## 3. Key decisions

* **Keep the C++ engine.** The matcher and gallery stay in C++. A Go rewrite
  adds GC pauses and accuracy risk with no benefit.
* **Go owns the public API.** The C++ REST server becomes an internal node and
  is removed once the gateway covers its API.
* **CPU cascade for search.** A bit-signature scan selects candidates and the
  existing matcher makes the final decision. Rejected for now: approximate
  vector indexes (fallback if the gate in section 5 fails) and GPU scanning
  (largest rewrite, GPUs needed in CI and the demo).
* **Fail closed.** An incomplete search or unaudited operation returns an error,
  never a false "no match". This trades some availability during outages for
  correct answers.
* **Honest capacity claims.** Every number is labelled by evidence level
  (section 11). 1,000 searches/s at 10M stays "projected" until run on a real
  cluster.

Capacity estimates, to be replaced with measurements: about 500 cores per
search/s for a 10M full scan, and about 600-1,500 cores in total for 1,000/s with
the cascade at 99.9% filtering.

## 4. Architecture

```
 React web app      React Native app       Devices / API clients
       └───────────────────┼───────────────────────┘
                           ▼
            Load balancer (TLS termination)
                           ▼
       Go gateway × M (stateless, horizontally scaled)
       auth · validation · rate limits · admission control
       audit · metrics · fan-out and merge
             │                          │
   enrol/delete (by subject)     identify (all shards)
             ▼                          ▼
  ┌──────────────────────────────────────────────────┐
  │ 256 virtual shards mapped onto fpx-node processes│
  │ each virtual shard has 2+ replicas               │
  │                                                  │
  │ fpx-node (C++, gRPC, internal only)              │
  │   Stage 1: bit-signature scan → top K            │
  │   Stage 2: existing matcher → final scores       │
  │   Write-ahead log with fsync → RAM gallery       │
  └──────────────────────────────────────────────────┘
             ▲                          ▲
   shard map, leases, health      PostgreSQL
   (etcd)                         subject records, audit
```

| Component | Location | Responsibility |
|---|---|---|
| Engine library | `src/core`, `include/fpx` | Existing code, plus signatures, batching, WAL, snapshots, explain mode |
| Node | `src/node` | gRPC service owning virtual shards |
| Gateway | `services/gateway` | Public REST API, replacing `examples/go-gateway` |
| Web app | `apps/web` | React demo |
| Mobile app | `apps/mobile` | React Native demo |
| Shared client | `packages/api-client` | TypeScript client and types generated from OpenAPI |
| Simulator | `tools/simulator` | Go discrete-event capacity simulator |
| Load tests | `tests/load` | k6 scenarios |

Shard size is set from milestone 2 measurements.

## 5. Search pipeline

```
Probe finger(s)
  ▼
Gateway: validate → admission check → one replica per shard
  ▼
fpx-node, per shard:
  1. Compile the probe and compute its bit signature
  2. Join a micro-batch (5 ms maximum wait, tunable)
  3. Stage 1: scan signatures of live, position-compatible fingers;
     keep top K per probe finger
  4. Stage 2: run the existing matcher on those candidates
  5. Fuse per subject with the existing FusionPolicy → top-k
  ▼
Gateway: all shards answered → merge → response
         any shard missing   → 503 incomplete
```

* **Signature:** fixed-size bits in a packed array, compared by Hamming distance,
  with SIMD where available. Milestone 2 tests 32, 64 and 128 bytes. The encoding
  is our own, built from the rotation- and translation-invariant local structures
  the matcher already computes. MCC is not used without a legal check of its
  patent and licence status.
* **Top K:** a per-shard percentage set per node. It may rise with spare capacity
  and never falls below the validated floor. Under load the node returns `429`
  rather than searching less.
* **Multi-finger:** each probe finger gets its own Stage 1 list. Lists are combined
  by subject, so one strong finger still reaches Stage 2. The two-finger policy is
  unchanged, and thresholds are recalibrated on public data.
* **Concurrency:** several searches run at once per node, and waiting probes share
  scans through micro-batching.

### Go/no-go gate (milestone 2)

Measured on public datasets, as the percentage of full-scan matches that survive
the cascade:

| Result | Action |
|---|---|
| ≥ 99.5% at 99.9% filtering | Pass |
| ≥ 99.5% only at 99% filtering | Pass, with about 5,000 estimated cores for Stage 2; report the cost before continuing |
| < 99.5% at 99% filtering | Stop and revisit approximate vector search before building the cluster |

## 6. Client flows

| | Option 1: on-device (default) | Option 2: server-side |
|---|---|---|
| For | Devices and apps that can run `libfpx` | Thin clients without native code |
| On the device | BMP → WSQ → MINEX | Capture BMP or WSQ |
| Sent to the API | MINEX template; WSQ only if archiving is on | The image |
| On the server | Matching | Image → WSQ if needed → MINEX → matching |
| Trade-off | Small payload, no raw image in transit, less server CPU | Simple client, raw images in transit, extraction capacity needed |

* MINEX means ANSI-378. It becomes the API default, replacing ISO. ISO templates
  remain accepted.
* The conversion order is BMP → WSQ → decode → MINEX, as requested. WSQ is lossy,
  so milestone 2 measures accuracy against extracting directly from BMP and
  reports the difference for a decision.
* Server-side extraction uses a separate worker pool, so extraction load cannot
  starve matching.

## 7. Writes and data flow

1. The gateway finds the subject's virtual shard and its leader replica.
2. The leader assigns a sequence number. Leadership is a lease held in etcd.
3. Replicas append to their WAL and fsync, grouping fsyncs for throughput.
4. After a quorum confirms, the client receives its response.
5. Each node publishes the change to memory, and the template becomes searchable.

Writes take an idempotency key, so retries are safe. A lagging replica may briefly
lack a new subject. A request with `min_version` goes only to replicas that have
caught up.

## 8. Failure handling

Rules: a search never reports "no match" unless every shard answered; a write is
never acknowledged without a quorum fsync; overload gets a fast rejection, never an
unbounded queue.

| Failure | Response | Client sees |
|---|---|---|
| Slow replica | Hedged request after about p95 latency; first answer wins | Slightly slower response |
| Replica down | Removed from routing | Nothing, if capacity allows |
| All replicas of a shard down | Searches fail; only that shard's writes fail | `503 incomplete` / `503` |
| No write quorum | Write rejected | `503` with `Retry-After` |
| Node crash | Restart, replay log, catch up, rejoin | Nothing, if replicas are healthy |
| Torn log tail | Truncate, recover from peers | Nothing |
| Mid-log corruption | Node refuses to serve | Nothing, if replicas are healthy |
| Overload | Per-shard in-flight limit | `429` with `Retry-After` |
| Gateway down | Load balancer reroutes | Retry of in-flight requests |
| Malformed input | Rejected before the matcher | `400` |
| etcd down | Gateways use the last known map; rebalancing and leader changes pause | Nothing, except writes to a shard needing a new leader |
| PostgreSQL down | Writes fail; searches fail closed by default, configurable | `503` |

Operations:

* **Restart:** snapshots plus streaming log replay. Target: a 10M-template restart
  under 5 minutes across the cluster, to be measured.
* **Shutdown:** stop new work, finish in-flight searches, exit.
* **Upgrades:** one replica per shard at a time. Ready only after catching up.
* **Rebalancing:** copy a snapshot, catch up, then switch the map, so no search
  skips or double-counts a shard.

## 9. Web app

React, Vite and TypeScript, in `apps/web`. `docker compose up` starts it with a
gateway, nodes and a small seeded synthetic gallery.

| Screen | Actions | Shows |
|---|---|---|
| Capture | Generate a synthetic finger, or upload BMP/PGM/TIFF/WSQ | Preview, BMP → WSQ → MINEX steps, minutiae overlay, quality warnings |
| Enrol | Create a subject, add fingers by position, choose Option 1 or 2 | Fingers, confirming replicas |
| Identify | Search with one or two fingers, a new impression or a stranger | Candidates, per-finger scores, policy decision |
| Verify | Check a finger against a subject | Score against threshold, matched minutiae side by side |
| Pipeline | Open any search result | Gallery → Stage 1 → Stage 2 → decision, with timing per stage and shard |
| Cluster | Kill or slow a node, start a traffic burst (local demo only) | Shard map, health, throughput, latency, hedges, `429`s |
| Simulation | Set gallery size, rate, filter rate, nodes | Projected cores, latency, capacity |

Engine and API additions:

* `/v1/extract` returns minutiae coordinates and angles, not just a count.
* The matcher gains an explain mode returning matched pairs, off the search path
  unless requested.
* Search responses gain optional per-stage timings and counts.
* The backend returns PNG previews, because browsers cannot display PGM, TIFF or WSQ.
* The gateway issues short-lived demo tokens, because a browser cannot hold the API key.
* Live cluster data streams over server-sent events.

Option 1 in the browser uses server-assisted conversion for the demo, shown as each
step. `libfpx` is not compiled to WebAssembly in this version, and the screen says
so. Real on-device conversion is demonstrated in the mobile app.

Safety: synthetic generation and fault controls need `DEMO_MODE`, which is off by
default and refused under the production profile. Uploads are processed in memory
and not stored. Only enrolled templates are saved. A banner warns against uploading
real people's fingerprints. Every number is tagged measured, synthetic or projected.

## 10. Mobile app

React Native in `apps/mobile`, sharing `packages/api-client` with the web app.

| Capability | Android | iOS |
|---|---|---|
| Synthetic fingers and image files | Yes | Yes |
| `libfpx` on the device (Option 1) | Yes | Yes |
| Server-side conversion (Option 2) | Yes | Yes |
| Live USB scanner capture | Yes, through the vendor SDK over USB OTG | No; revisit if the vendor provides an iOS SDK |

Screens: capture, enrol, identify, verify, settings.

* Scanners sit behind a `FingerprintSource` interface, with synthetic, file and USB
  implementations.
* Built-in phone sensors and cameras are excluded. Touch ID and BiometricPrompt
  never expose images, and photos are not comparable to scanner images.
* **Licence risk:** FingerJet is LGPL-3.0. iOS builds stay internal until legal
  review clears distribution. Android distribution must still meet LGPL terms.

## 11. Testing and evidence

| Layer | Tools | Runs |
|---|---|---|
| C++ unit | doctest | Every push |
| Go unit | `go test -race` | Every push |
| Web and mobile unit | Vitest, Testing Library, Jest | Every push |
| Robustness | ASan, UBSan, TSan; libFuzzer, Go fuzzing | Sanitizers every push, fuzzing nightly |
| Contract | Generated gRPC and OpenAPI schemas | Every push |
| Integration | Docker Compose with real gateway, nodes, etcd, PostgreSQL | Every push |
| Chaos | Node kills, Toxiproxy, covering section 8 | Small set per PR, full set nightly |
| Visual | Playwright screenshots with seeded data, light and dark | Every PR |
| End-to-end, web | Playwright against the full stack | Every PR |
| End-to-end, mobile | Maestro on emulators | Nightly |
| Scanner | Simulated source in CI; manual checklist on real hardware | Per release |
| Accuracy | `fpx eval`; public datasets stored outside git | Synthetic on PRs, public datasets locally or on private storage |

Core chaos assertions: no acknowledged write is lost, and no incomplete search
returns "no match".

**Load tests (k6 through Docker):** steady, ramp, spike, soak, enrol-heavy, mixed, and
node failure under load. Initial targets at rated load: p99 identify under 2 s, under
1% errors excluding deliberate `429`s. A separate test measures connection capacity
per gateway, and the runbook covers OS limits and load generators for 1M connections.

**Simulator:** Go discrete-event model of arrivals, batching, shards, replicas,
hedging, admission control and failures. It uses measured distributions only and must
reproduce a real 3-node cluster within 15% before its projections are published.

**CI:** GitHub Actions, with a PR path of about 15 minutes and a nightly job. A
performance drop over 10% from the stored baseline is flagged.

| Level | Evidence | Label |
|---|---|---|
| 1 | One node on real hardware | Measured |
| 2 | Small cluster, synthetic data | Measured, small scale |
| 3 | Simulator calibrated against level 2 | Projected |
| 4 | Real cluster at 10M and 1,000 searches/s | Proven at target |

Level 4 needs a cloud account and budget from the project owner. The repo supplies
the runbook and scripts.

## 12. Documentation

* `docs/ARCHITECTURE.md`: rewritten with component, data-flow and failure diagrams.
* `docs/GETTING_STARTED.md`: step-by-step from clone to working demo.
* `docs/SCALING.md`: replaced with measured results and evidence labels.
* `docs/OPERATIONS.md`: deployment, upgrades, recovery, and the level 4 runbook.
* `docs/API.md`, generated from OpenAPI, covering both client options.
* `docs/MOBILE.md`: builds, scanner integration, licensing.

## 13. Milestones

Each gets its own implementation plan and adds its own tests as it goes.

| # | Milestone | Done when |
|---|---|---|
| 1 | Docs and baseline | Diagrams, guide, and benchmark and accuracy baselines committed |
| 2 | Pre-filter experiment and WSQ impact | Gate result and WSQ measurement reported |
| 3 | Engine hardening | Concurrent search, fsync WAL, snapshots, gRPC node |
| 4 | Go gateway and clustering | Section 8 chaos tests pass on a local cluster |
| 5 | React web app | All screens covered by visual and end-to-end tests |
| 6 | Mobile app | Android and iOS flows pass on emulators; USB capture passes on real hardware |
| 7 | Load tests and simulation | Levels 1-3 reported; level 4 runbook ready |

## 14. Inputs needed from the project owner

| Input | Needed by | If absent |
|---|---|---|
| FVC-style public datasets, with licences confirmed for this use | Milestone 2 | Gate cannot be passed; results stay synthetic only |
| Scanner model and Android vendor SDK | Milestone 6 | USB capture stays behind the interface, untested on hardware |
| Apple developer account | Milestone 6 | iOS is limited to the simulator |
| Legal review of FingerJet LGPL on iOS, and of any use of MCC | Before iOS distribution | iOS stays internal; MCC stays unused |
| Cloud account and budget | Level 4 | Target remains projected |

## 15. Risks

| Risk | Mitigation |
|---|---|
| Signature misses too many real matches | Gate in milestone 2, before cluster spending |
| Public datasets are far smaller than 10M | Report real-finger and scale evidence separately |
| False matches grow with gallery size on real fingers | Recalibrate thresholds; recommend two-finger search; state evidence limits |
| WSQ-first conversion reduces accuracy | Measure in milestone 2; owner decides |
| Actual cost exceeds estimates | Levels 1-3 replace estimates before level 4 spending |
| Scope is large | Milestones ship independently, and work stops cleanly at each one |
