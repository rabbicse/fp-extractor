# Roadmap

Full design: [scale platform spec](superpowers/specs/2026-11-02-fp-extractor-scale-design.md).
Next plan: [milestones 1-2](superpowers/plans/2026-11-02-scale-m1-m2-baseline-and-gate.md).

| Stage | Scope | Status |
|---|---|---|
| Demo slice | React demo on today's engine, upload formats, cross-match, capacity tables | Built, awaiting your review |
| 1 | Docs and baselines | Partly done |
| 2 | Variant lab, pre-filter experiment, WSQ impact, go/no-go gate | Planned |
| 3 | Concurrent search, fsync log, snapshots, gRPC node | Planned |
| 4 | Go gateway, replicas, virtual shards, overload protection | Planned |
| 5 | Complete web app, desktop scanner agent | Planned |
| 6 | Android and iOS app | Planned |
| 7 | Load tests and calibrated simulation | Planned |

## What works today

- BMP, WSQ, PGM and TIFF input, MINEX and ISO templates
- Enrolment, 1:1 and 1:N matching with up to 10 fingers, on one server
- The web demo, in `apps/web`

## What is planned, not built

- Clustering, replicas and failover
- The pre-filter, so 1,000 searches/s at 10M is a design target
- DigitalPersona and Futronic live capture, and the mobile app
- Accuracy measured on real scanner data
