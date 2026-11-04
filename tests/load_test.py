#!/usr/bin/env python3
"""HTTP load test for fpx-server (or the Go gateway). Python standard library only.

Prepare a synthetic gallery and probes, start a server on it, then run this:

    build/fpx gendb --db load.fpxdb --subjects 100000 --fingers 2 --probes 40 --probe-dir load-probes
    build/fpx-server --db load.fpxdb --port 8080 &
    python3 tests/load_test.py --url http://127.0.0.1:8080 --probes load-probes --concurrency 1,4,16 --duration 20

It first sends every probe once, one at a time, to record the reference answers
and the accuracy (genuine probes accepted as the right subject, impostors
rejected). Then, for every concurrency level, it sends POST /v1/identify for
--duration seconds and reports throughput and latency percentiles.

Exit code 1 if any request failed, if any answer under load differs from the
reference answer (a concurrency bug), or on any false match (a probe accepted
as the wrong subject, or an impostor accepted). A genuine probe that is not
accepted is a false non-match: reported as a rate, it fails the run only above
--max-fnmr. With --max-p95-ms the run also fails when p95 latency is too high.
"""
import argparse
import base64
import http.client
import json
import os
import sys
import threading
import time
import urllib.parse


def load_probes(directory):
    with open(os.path.join(directory, "probes.json")) as f:
        manifest = json.load(f)
    probes = []
    for p in manifest:
        fingers = []
        for fg in p["fingers"]:
            with open(os.path.join(directory, fg["file"]), "rb") as t:
                fingers.append({"position": fg["position"], "template": base64.b64encode(t.read()).decode()})
        probes.append((p["expect"], json.dumps({"fingers": fingers, "top_k": 5}).encode()))
    return probes


def percentile(sorted_values, q):
    if not sorted_values:
        return 0.0
    i = min(len(sorted_values) - 1, max(0, int(round(q / 100.0 * (len(sorted_values) - 1)))))
    return sorted_values[i]


def identify_once(url, api_key, body, timeout):
    u = urllib.parse.urlsplit(url)
    conn = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=timeout)
    conn.request("POST", "/v1/identify", body=body,
                 headers={"Content-Type": "application/json", **({"X-API-Key": api_key} if api_key else {})})
    resp = conn.getresponse()
    data = resp.read()
    conn.close()
    if resp.status != 200:
        raise RuntimeError(f"HTTP {resp.status}: {data[:200]!r}")
    out = json.loads(data)
    return out.get("id") if out.get("match") else None


def run_level(url, api_key, probes, reference, concurrency, duration, timeout):
    u = urllib.parse.urlsplit(url)
    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["X-API-Key"] = api_key
    lock = threading.Lock()
    lat, errors, wrong = [], [], []
    counter = [0]
    deadline = time.monotonic() + duration

    def worker():
        conn = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=timeout)
        local_lat = []
        while time.monotonic() < deadline:
            with lock:
                k = counter[0] % len(probes)
                counter[0] += 1
            body = probes[k][1]
            t0 = time.perf_counter()
            try:
                conn.request("POST", "/v1/identify", body=body, headers=headers)
                resp = conn.getresponse()
                data = resp.read()
                ms = (time.perf_counter() - t0) * 1000
                if resp.status != 200:
                    with lock:
                        errors.append(f"HTTP {resp.status}: {data[:120]!r}")
                    continue
                out = json.loads(data)
                local_lat.append(ms)
                got = out.get("id") if out.get("match") else None
                if got != reference[k]:
                    with lock:
                        wrong.append(f"probe {k}: answered {got} under load, {reference[k]} sequentially")
            except Exception as e:  # connection reset, timeout, bad JSON
                with lock:
                    errors.append(f"{type(e).__name__}: {e}")
                conn.close()
                conn = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=timeout)
        conn.close()
        with lock:
            lat.extend(local_lat)

    t_start = time.monotonic()
    threads = [threading.Thread(target=worker) for _ in range(concurrency)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t_start
    lat.sort()
    return {
        "concurrency": concurrency,
        "requests": len(lat) + len(errors),
        "ok": len(lat),
        "errors": len(errors),
        "wrong": len(wrong),
        "rps": len(lat) / elapsed if elapsed > 0 else 0.0,
        "p50_ms": percentile(lat, 50),
        "p95_ms": percentile(lat, 95),
        "p99_ms": percentile(lat, 99),
        "max_ms": lat[-1] if lat else 0.0,
        "error_samples": errors[:3],
        "wrong_samples": wrong[:3],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default=os.environ.get("FPX_URL", "http://127.0.0.1:8080"))
    ap.add_argument("--api-key", default=os.environ.get("FPX_API_KEY", ""))
    ap.add_argument("--probes", required=True, help="directory written by `fpx gendb --probe-dir`")
    ap.add_argument("--concurrency", default="1,4,16", help="comma-separated client counts")
    ap.add_argument("--duration", type=float, default=15, help="seconds per concurrency level")
    ap.add_argument("--timeout", type=float, default=120, help="per-request timeout, seconds")
    ap.add_argument("--max-fnmr", type=float, default=0.10, help="fail if more genuine probes are rejected (default 0.10)")
    ap.add_argument("--max-p95-ms", type=float, default=0, help="fail if p95 latency exceeds this (0 = no limit)")
    ap.add_argument("--json", help="also write the results to this file")
    args = ap.parse_args()

    probes = load_probes(args.probes)
    genuine = sum(1 for e, _ in probes if e is not None)
    print(f"{len(probes)} probes ({genuine} genuine, {len(probes) - genuine} impostor) -> {args.url}/v1/identify")

    # Reference pass: one request at a time.
    reference, fnm, fm = [], [], []
    for expect, body in probes:
        got = identify_once(args.url, args.api_key, body, args.timeout)
        reference.append(got)
        if got is not None and got != expect:
            fm.append(f"expected {expect}, accepted as {got}")
        elif expect is not None and got is None:
            fnm.append(expect)
    fnmr = len(fnm) / genuine if genuine else 0.0
    print(f"accuracy: {genuine - len(fnm)}/{genuine} genuine accepted (FNMR {fnmr:.1%}), "
          f"{len(fm)} false match(es) over {len(probes)} probes")
    for s in fm:
        print(f"        ! false match: {s}")
    failed = bool(fm) or fnmr > args.max_fnmr
    if fnmr > args.max_fnmr:
        print(f"        ! FNMR {fnmr:.1%} exceeds --max-fnmr {args.max_fnmr:.1%}")

    print(f"{'clients':>7} {'requests':>8} {'req/s':>7} {'p50 ms':>8} {'p95 ms':>8} {'p99 ms':>8} {'max ms':>8} {'errors':>6} {'diff':>5}")
    results = []
    for c in [int(x) for x in args.concurrency.split(",") if x.strip()]:
        r = run_level(args.url, args.api_key, probes, reference, c, args.duration, args.timeout)
        results.append(r)
        print(f"{r['concurrency']:>7} {r['requests']:>8} {r['rps']:>7.2f} {r['p50_ms']:>8.1f} {r['p95_ms']:>8.1f} "
              f"{r['p99_ms']:>8.1f} {r['max_ms']:>8.1f} {r['errors']:>6} {r['wrong']:>5}")
        for s in r["error_samples"] + r["wrong_samples"]:
            print(f"        ! {s}")
        if r["errors"] or r["wrong"] or r["ok"] == 0:
            failed = True
        if args.max_p95_ms and r["p95_ms"] > args.max_p95_ms:
            print(f"        ! p95 {r['p95_ms']:.1f} ms exceeds --max-p95-ms {args.max_p95_ms}")
            failed = True
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"url": args.url, "probes": len(probes), "genuine": genuine, "fnmr": fnmr,
                       "false_matches": len(fm), "results": results}, f, indent=2)
    print("LOAD TEST " + ("FAILED" if failed else "PASSED"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
