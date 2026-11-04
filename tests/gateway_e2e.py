#!/usr/bin/env python3
"""End-to-end test of the Go sharding gateway (examples/go-gateway) in front of
fpx-server shards. Python standard library only.

    python3 tests/gateway_e2e.py --gateway http://127.0.0.1:9090 --demo fpx-demo \
        [--api-key K] [--shards http://127.0.0.1:8081,http://127.0.0.1:8082 --shard-key S]

--demo is a folder written by `fpx demo --out DIR` (finger{1,2,3}_{0,1}.iso).
With --shards it also checks that the subjects are spread over the shards.
Exit code 0 when every check passes.
"""
import argparse
import base64
import json
import os
import sys
import urllib.error
import urllib.request

failures = 0


def check(ok, what):
    global failures
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures += 1


def call(method, url, body=None, key=""):
    req = urllib.request.Request(url, data=json.dumps(body).encode() if body is not None else None, method=method,
                                 headers={"Content-Type": "application/json", **({"X-API-Key": key} if key else {})})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gateway", required=True)
    ap.add_argument("--demo", required=True)
    ap.add_argument("--api-key", default="")
    ap.add_argument("--shards", default="")
    ap.add_argument("--shard-key", default="")
    a = ap.parse_args()
    gw, key = a.gateway.rstrip("/"), a.api_key
    tmpl = lambda f, i: base64.b64encode(open(os.path.join(a.demo, f"finger{f}_{i}.iso"), "rb").read()).decode()
    finger = lambda f, i: {"fingers": [{"position": 2, "template": tmpl(f, i)}]}
    ids = [f"gw-e2e-{f}" for f in (1, 2, 3)]

    for f, sid in zip((1, 2, 3), ids):
        st, _ = call("PUT", f"{gw}/v1/subjects/{sid}", finger(f, 0), key)
        check(st in (200, 201), f"enrol {sid} through the gateway (HTTP {st})")
    if a.shards:
        counts = []
        for s in a.shards.split(","):
            st, out = call("GET", f"{s.rstrip('/')}/v1/stats", key=a.shard_key)
            counts.append(out["subjects"] if st == 200 else -1)
        check(all(c >= 0 for c in counts) and sum(counts) >= 3, f"subjects stored on the shards: {counts}")
        check(sum(1 for c in counts if c > 0) >= 2, "subjects are spread over more than one shard")
    for f, sid in zip((1, 2, 3), ids):
        st, out = call("POST", f"{gw}/v1/identify", finger(f, 1), key)
        check(st == 200 and out["match"] and out["id"] == sid, f"identify second impression of finger {f} -> {sid}")
    st, out = call("POST", f"{gw}/v1/verify", {"id": ids[1], **finger(2, 1)}, key)
    check(st == 200 and out["match"], "verify genuine finger")
    st, out = call("POST", f"{gw}/v1/verify", {"id": ids[1], **finger(1, 1)}, key)
    check(st == 200 and not out["match"], "verify impostor finger is rejected")
    st, out = call("GET", f"{gw}/v1/subjects/{ids[1]}", key=key)
    check(st == 200 and out["id"] == ids[1], "GET subject through the gateway")
    if key:
        st, _ = call("POST", f"{gw}/v1/identify", finger(1, 1), "wrong-key")
        check(st == 401, f"wrong client key is refused (HTTP {st})")
    for sid in ids:
        st, _ = call("DELETE", f"{gw}/v1/subjects/{sid}", key=key)
        check(st == 200, f"delete {sid}")
    st, out = call("POST", f"{gw}/v1/identify", finger(1, 1), key)
    check(st == 200 and not out["match"], "deleted subject is no longer identified")

    print("gateway e2e: " + ("PASSED" if failures == 0 else f"{failures} FAILED"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
