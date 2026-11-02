# Capacity and hardware estimates

Every number has one of three labels:

- **Measured:** run on real hardware with synthetic templates.
- **Estimated:** calculated from the measurements below.
- **Not built:** depends on planned work that does not exist yet.

Accuracy on real scanner data at these scales has not been measured.

## Measured benchmarks

Apple M1, 8 threads, 16 GB RAM, today's engine, 2026-11-02. Searches run one at a time.

| Gallery | Average search | Worst search | RAM | Build time | Rank-1 correct | Best impostor score | Probes |
|---|---|---|---|---|---|---|---|
| 1,000 templates | 0.025 s | 0.054 s | 3 MB | under 0.1 s | not recorded | not recorded | 10 |
| 100,000 templates | 0.395 s | 1.089 s | 292 MB | 0.9 s | not recorded | not recorded | 10 |
| 200,000 templates | 1.274 s | 2.644 s | 583 MB | 1.7 s | 10/10 | 9.0 | 10 |
| 1M templates | 5.03 s | 10.38 s | 1.5 GB | 7.6 s | 10/10 | 12.2 | 10 |
| 2M templates | 15.58 s | 23.34 s | 3.0 GB | 42.3 s | 5/5 | 12.2 | 5 |
| 5M templates | 37.19 s | 54.18 s | 7.5 GB | 49.2 s | 5/5 | 9.0 | 5 |
| 1M customers × 2 fingers | 11.58 s | 13.71 s | 2.9 GB | 19.1 s | 5/5 accepted | 12.2, no false alarms | 5 |

These are small probe counts on a laptop, with other work running on some of them, so treat them
as indicative. Speed ranged from 130,000 to 200,000 comparisons/s across runs. Impostor results come
from only 1-2 non-enrolled probes per run, far too few to establish a false-match rate.

### Conversion, one sample synthetic print

| Step | Size |
|---|---|
| BMP, 320 × 400 px, 500 dpi | 126.1 KB |
| WSQ at 0.75 bitrate | 9.0 KB |
| MINEX (ANSI-378), 89 minutiae | 567 B |
| ISO-19794-2 | 564 B |

Real scanner sizes will differ. Measuring them is part of [milestone 2](ROADMAP.md).

## Estimated hardware, today's full scan

**Estimated.** Formula: `search seconds = templates / (cores × 16,250)`. That rate is the slowest measured
run (130,000 comparisons/s on 8 threads). Server CPUs may be faster or slower, so measure with
`fpx bench` on the target machine.

RAM is for one copy. The range runs from measured synthetic templates to double that, as real templates can hold more minutiae. Multiply by the replica count.

| Templates | RAM | Search, 16 cores | Search, 32 cores | Search, 64 cores | Cores for 1 search/s |
|---|---|---|---|---|---|
| 1M | 2-3 GB | 3.8 s | 1.9 s | 1.0 s | 62 |
| 2M | 3-6 GB | 7.7 s | 3.8 s | 1.9 s | 124 |
| 5M | 8-15 GB | 19.2 s | 9.6 s | 4.8 s | 308 |
| 10M | 15-31 GB | 38.5 s | 19.2 s | 9.6 s | 616 |
| 20M | 31-62 GB | 76.9 s | 38.5 s | 19.2 s | 1,231 |
| 50M | 77-154 GB | 192 s | 96 s | 48 s | 3,077 |
| 100M | 154-308 GB | 385 s | 192 s | 96 s | 6,154 |

Doubling cores assumes near-linear scaling, which has not been tested above 8 threads.

## Estimated hardware for 1,000 searches/s

**Not built.** This depends on the planned pre-filter, and its accuracy is unproven until the
[go/no-go gate](superpowers/specs/2026-11-02-fp-extractor-scale-design.md#gono-go-gate-milestone-2).
Figures cover Stage 2 only. Stage 1, gateways and spare capacity are extra.

| Templates | Full scan, no filter | Stage 2 at 99% filter | Stage 2 at 99.9% filter |
|---|---|---|---|
| 1M | 61,539 cores | 616 | 62 |
| 2M | 123,077 cores | 1,231 | 124 |
| 5M | 307,693 cores | 3,077 | 308 |
| 10M | 615,385 cores | 6,154 | 616 |
| 20M | 1,230,770 cores | 12,308 | 1,231 |
| 50M | 3,076,924 cores | 30,770 | 3,077 |
| 100M | 6,153,847 cores | 61,539 | 6,154 |

## Estimated storage, one copy

| Templates | Templates on NVMe (up to 1.5 KB each) | WSQ in object storage (10-30 KB each) |
|---|---|---|
| 1M | 1.5 GB | 10-30 GB |
| 2M | 3 GB | 20-60 GB |
| 5M | 7.5 GB | 50-150 GB |
| 10M | 15 GB | 100-300 GB |
| 20M | 30 GB | 200-600 GB |
| 50M | 75 GB | 500-1,500 GB |
| 100M | 150 GB | 1-3 TB |

## Reproduce

```bash
build/fpx bench --gallery 1000000 --probes 10
build/fpx bench --gallery 1000000 --probes 10 --fingers 2
```

The estimate formulas live in `apps/web/src/capacity.ts`, with unit tests, and the web app's
Performance screen uses them.
