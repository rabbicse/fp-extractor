#!/usr/bin/env python3
"""End-to-end test of fpx-server (run by ctest; standard library only).

usage: server_test.py <fpx binary> <fpx-server binary> <work dir>
"""
import base64
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

FPX, SERVER, WORK = sys.argv[1], sys.argv[2], sys.argv[3]
KEY = "test-key"
os.makedirs(WORK, exist_ok=True)
DB = os.path.join(WORK, "server_test.fpxdb")
for p in (DB,):
    if os.path.exists(p):
        os.remove(p)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


PORT = free_port()
URL = f"http://127.0.0.1:{PORT}"


def start():
    proc = subprocess.Popen([SERVER, "--db", DB, "--host", "127.0.0.1", "--port", str(PORT), "--api-key", KEY],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(100):
        try:
            urllib.request.urlopen(URL + "/healthz", timeout=1)
            return proc
        except Exception:
            time.sleep(0.1)
    proc.kill()
    raise SystemExit("server did not start: " + proc.stderr.read().decode())


def call(method, path, body=None, key=KEY):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"}
    if key:
        headers["X-API-Key"] = key
    req = urllib.request.Request(URL + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, json.load(e)


def synth(subject, position, impression):
    """Synthetic scanner image for (subject, finger position, impression)."""
    path = os.path.join(WORK, f"s{subject}_f{position}_{impression}.bmp")
    if not os.path.exists(path):
        subprocess.run([FPX, "synth", "-o", path, "--finger", str(subject * 16 + position),
                        "--impression", str(impression)], check=True, stdout=subprocess.DEVNULL)
    return path


def template(subject, position, impression):
    img = synth(subject, position, impression)
    out = img[:-4] + ".iso"
    if not os.path.exists(out):
        subprocess.run([FPX, "extract", img, "-o", out], check=True, stdout=subprocess.DEVNULL)
    return out


def b64(path):
    return base64.b64encode(open(path, "rb").read()).decode()


failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


proc = start()
try:
    st, _ = call("GET", "/v1/stats", key=None)
    check(st == 401, "request without API key is rejected (401)")

    # Enrol 3 customers with right index (2) as template and left index (7) as raw image.
    for s in (1, 2, 3):
        st, out = call("PUT", f"/v1/subjects/cust-{s}", {"fingers": [
            {"position": 2, "template": b64(template(s, 2, 0))},
            {"position": 7, "image": b64(synth(s, 7, 0))}]})
        check(st == 201 and len(out["fingers"]) == 2, f"enrol cust-{s} with two fingers (201)")

    st, out = call("GET", "/v1/subjects/cust-2")
    check(st == 200 and [f["position"] for f in out["fingers"]] == [2, 7], "GET subject lists fingers 2 and 7")

    st, out = call("POST", "/v1/identify", {"fingers": [
        {"position": 2, "template": b64(template(2, 2, 1))},
        {"position": 7, "template": b64(template(2, 7, 1))}], "top_k": 3})
    check(st == 200 and out["match"] and out["id"] == "cust-2", "two-finger identify finds cust-2")
    check(out["candidates"][0]["fingers_matched"] == 2, "both fingers matched")
    check(out["fingers_presented"] == 2 and out["compared"] == 6, "each probe finger compared only with its position")

    st, out = call("POST", "/v1/identify", {"position": 7, "image": b64(synth(3, 7, 1))})
    check(st == 200 and out["match"] and out["id"] == "cust-3", "legacy single-finger identify (image) finds cust-3")

    st, out = call("POST", "/v1/identify", {"fingers": [
        {"position": 2, "template": b64(template(9, 2, 1))},
        {"position": 7, "template": b64(template(9, 7, 1))}]})
    check(st == 200 and not out["match"] and out["id"] is None, "stranger is not identified")

    st, out = call("POST", "/v1/verify", {"id": "cust-1", "fingers": [
        {"position": 2, "template": b64(template(1, 2, 1))},
        {"position": 7, "template": b64(template(1, 7, 1))}]})
    check(st == 200 and out["match"] and out["fingers_matched"] == 2, "two-finger verify cust-1")
    st, out = call("POST", "/v1/verify", {"id": "cust-1", "fingers": [
        {"position": 2, "template": b64(template(2, 2, 1))},
        {"position": 7, "template": b64(template(2, 7, 1))}]})
    check(st == 200 and not out["match"], "verify cust-1 with cust-2's fingers fails")
    st, _ = call("POST", "/v1/verify", {"id": "nobody", "template": b64(template(1, 2, 1))})
    check(st == 404, "verify unknown id -> 404")

    # Per-finger management.
    st, out = call("PUT", "/v1/subjects/cust-1/fingers/3", {"template": b64(template(1, 3, 0)), "position": 9})
    check(st == 201 and [f["position"] for f in out["fingers"]] == [2, 3, 7], "add finger 3 (URL position wins)")
    st, _ = call("DELETE", "/v1/subjects/cust-1/fingers/3")
    check(st == 200, "delete finger 3")
    st, _ = call("DELETE", "/v1/subjects/cust-1/fingers/3")
    check(st == 404, "deleting it again -> 404")
    st, out = call("PUT", "/v1/subjects/cust-3", {"fingers": [{"position": 2, "template": b64(template(3, 2, 0))}]})
    check(st == 200 and [f["position"] for f in out["fingers"]] == [2], "PUT subject replaces its finger set")

    # Validation.
    st, out = call("PUT", "/v1/subjects/x", {"fingers": [{"position": 2, "template": b64(template(1, 2, 0))},
                                                         {"position": 2, "template": b64(template(1, 2, 1))}]})
    check(st == 400 and "twice" in out["error"], "duplicate finger position -> 400")
    st, out = call("PUT", "/v1/subjects/x", {"fingers": [{"position": 11, "template": b64(template(1, 2, 0))}]})
    check(st == 400, "finger position 11 -> 400")
    st, _ = call("PUT", "/v1/subjects/x", {"template": "AAAA"})
    check(st == 400, "garbage template -> 400")

    # v2.0 compatibility: POST /v1/subjects/{id} with a bare template (finger slot 0).
    st, out = call("POST", "/v1/subjects/legacy", {"template": b64(template(4, 2, 0))})
    check(st == 201 and out["fingers"][0]["position"] == 0, "legacy single-template enrolment -> slot 0")
    st, out = call("POST", "/v1/identify", {"template": b64(template(4, 2, 1))})
    check(out["id"] == "legacy", "legacy subject found by an unlabelled probe")

    st, _ = call("DELETE", "/v1/subjects/cust-2")
    check(st == 200, "delete cust-2")
    st, stats = call("GET", "/v1/stats")
    check(stats["subjects"] == 3 and stats["templates"] == 4, f"stats: 3 subjects / 4 templates ({stats})")

    st, out = call("POST", "/v1/admin/compact")
    check(st == 200 and out["bytes_after"] < out["bytes_before"], f"online compaction shrinks the store ({out})")
    st, out = call("PUT", "/v1/subjects/after-compact", {"template": b64(template(5, 2, 0)), "position": 2})
    check(st == 201, "enrolment after compaction")
    st, _ = call("DELETE", "/v1/subjects/after-compact")
    check(st == 200, "delete after compaction")
finally:
    proc.terminate()
    proc.wait(timeout=10)

# Persistence: restart and check the same state comes back.
proc = start()
try:
    st, stats = call("GET", "/v1/stats")
    check(stats["subjects"] == 3 and stats["templates"] == 4, "state restored after restart")
    st, out = call("GET", "/v1/subjects/cust-1")
    check(st == 200 and [f["position"] for f in out["fingers"]] == [2, 7], "cust-1 fingers restored")
    st, _ = call("GET", "/v1/subjects/cust-2")
    check(st == 404, "deleted subject stays deleted")
finally:
    proc.terminate()
    proc.wait(timeout=10)

print(f"\n{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
