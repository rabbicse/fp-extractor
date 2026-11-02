# Scaling 1:N identification to 1 M+ customers

> These figures come from one 4-vCPU run with synthetic templates. Today's server runs one search at a
> time per process. See [CAPACITY.md](CAPACITY.md) for newer measurements and hardware estimates, and
> the [roadmap](ROADMAP.md) for planned scale work.

Target: identify a probe against **1,000,000** enrolled templates in
**≤ 30–60 s**. Measured result: **~5 s on 4 vCPUs**, so the target is met with
a large margin. On a typical 16–32 core server expect roughly 1–2 s
(the scan parallelises across cores; verify on your hardware with `fpx bench`).

## Measured (this repository, 4-vCPU cloud container, GCC 13, -O3)

```
$ fpx bench --gallery 1000000 --probes 40
gallery ready: 1000000 templates, ~1502 MB, built in 15.7 s
1:N search over 1000000 templates (threads=4)
  avg latency:      5012.3 ms   (worst 9193.2 ms)
  throughput:       0.20 M comparisons/s
  rank-1 hit rate:  39 / 40  (correct id AND score >= identify threshold 18)
  genuine score:    min 24.5  mean 45.8   (over the 39 hits)
  best impostor score over the whole gallery (10 non-enrolled probes): max 16.0 -> no false alarm
peak RSS: ~1.5 GB
```

Read the accuracy lines honestly: with **one finger** and this synthetic
template model, the best of 1 M impostor scores reached 16.0, close to the
identify threshold of 18, and 1 of 40 genuine probes scored below 18 (the
right person was still rank 1). Single-finger 1:N at a million is hard for any
matcher. For production, **enrol and search two fingers per customer** and
require both to agree (or fuse scores); that pushes false matches down by
orders of magnitude and is standard practice for large AFIS systems. This is
built in since v2.1 (server, CLI, Go gateway); results below. See
[MATCHING.md](MATCHING.md#improving-accuracy-further-ideas-in-order-of-value).

### Two fingers per customer (recommended)

```
$ fpx bench --gallery 1000000 --probes 20 --fingers 2
gallery ready: 1000000 subjects, 2000000 templates, ~2997 MB, built in 26.6 s
2-finger 1:N search over 1000000 subjects / 2000000 templates (threads=4)
  policy:           both fingers >= 12, or one finger >= 30
  avg latency:      9867.2 ms   (worst 14600.1 ms)
  rank-1:           20 / 20 correct, 20 / 20 also accepted by the policy
  weakest genuine finger score (rank-1 hits): 15.8
  impostor searches (10 non-enrolled people): 0 false alarms; best single finger 15.2, best fused 15.2
```

Compared with one finger: every genuine customer is now accepted (single
finger: 39/40), and a stranger would need **both** fingers to coincidentally
reach 12 against the **same** customer. In 10 M stranger comparisons per
finger, no customer had even two fingers above 12 (best fused score 15.2 =
one finger only). Cost: twice the comparisons and RAM (each probe finger is
compared only with enrolled fingers of the same position), i.e. ~10 s on
4 cores, ~2.5 s on 16 cores for 1 M customers.

Rules of thumb (synthetic templates averaging 40 minutiae; real FingerJet
templates often have more, so budget up to 2×):

| Gallery | RAM | Search time, 4 cores | 16 cores | 64 cores (4 × 16 shards) |
|---|---|---|---|---|
| 100 k | 0.15 GB | 0.5 s | ~0.15 s | — |
| 1 M | 1.5 GB | 5 s | ~1.3 s | ~0.3 s |
| 10 M | 15 GB | 50 s | ~13 s | ~3 s |
| 1 M customers × 2 fingers | 3 GB | 10 s | ~2.5 s | ~0.6 s |

Run `fpx bench --gallery N` on the target machine to get real numbers.

## Why RAM + linear scan (and what the database is for)

A 1:N fingerprint search has no exact index: every enrolled template must be
compared with the probe. The fastest possible implementation is therefore a
sequential, multi-threaded scan over templates packed in memory: no DB round
trips, no deserialisation, perfect cache prefetching. Biometric vendors' AFIS
engines do exactly this.

The database is the **system of record**, not the search engine:

| Option | When |
|---|---|
| Built-in `TemplateStore` (`*.fpxdb`, append-only + CRC) | single node / per-shard persistence; zero dependencies; loads 1 M templates in seconds |
| **PostgreSQL** (`bytea` column, recommended) | you already have customer data there; transactions, backups, replication, audit. Matcher nodes load their shard at start-up (`SELECT id, template FROM templates WHERE shard = $1`) and get changes via `LISTEN/NOTIFY` or a queue |
| Redis / KeyDB | fast restart cache in front of PostgreSQL |
| ScyllaDB / Cassandra | 100 M+ templates, multi-datacentre |

"Faster databases" don't make matching faster; more cores and sharding do.

## Single node (≤ ~5 M templates)

```
clients ──► fpx-server (all cores) ──► gallery.fpxdb (append-only log)
```

* `--threads N` limits matcher threads (default: all cores).
* Searches are serialised on the matcher pool (each uses all cores), so
  throughput ≈ 1 / latency: ~0.2 searches/s at 1 M on 4 cores, ~0.8/s on
  16 cores. If you need more searches per second, add replicas (same data) behind a load
  balancer.

## Sharded cluster (10 M+, or high QPS)

```
                       ┌──► fpx-server shard 0 (templates with hash(id) % N == 0)
clients ─► Go gateway ─┼──► fpx-server shard 1
          (auth, DB,   └──► fpx-server shard N-1
           fan-out)        each: RAM gallery + own .fpxdb (or loads its slice from PostgreSQL)
```

* **Enrol / delete / verify**: routed to `hash(id) % N`.
* **Identify**: scatter to all shards concurrently, gather top-k, merge.
  Latency = slowest shard ≈ (gallery / N) / cores-per-shard.
* **Replicas**: run 2 copies of each shard for availability; the gateway
  picks one healthy replica per shard.
* **Re-sharding**: use consistent hashing or a fixed large number of virtual
  shards (e.g. 256) mapped onto nodes, so adding a node moves only a slice.
* Reference implementation: `examples/go-gateway` (standard library only,
  tested with 2 shards). `docker/docker-compose.yml --profile sharded`
  starts 2 shards + gateway.

## Capacity planning checklist

1. Measure on target hardware: `fpx bench --gallery <your size> --threads <cores>`.
2. RAM: ~1.5 KB × templates × 2 (headroom) per shard.
3. Pick shards so that `(templates per shard) / (cores × 50 k/s)` fits your
   latency budget with margin.
4. Start-up time: replaying 1 M templates takes ~15 s on 4 cores (template
   compilation); use rolling restarts with replicas.
5. Monitor: search latency p50/p99, gallery size, memory, false-match
   reports.

## Further speed-ups if ever needed

* Pre-filter stage (pattern class / orientation-field signature / binary
  descriptors) to drop 80–95 % of candidates before full matching.
* SIMD (AVX2) for the correspondence scan; cache-blocking several probes
  per pass to batch concurrent searches.
* Multiple fingers per customer: search with the best-quality finger first,
  confirm with the second.
