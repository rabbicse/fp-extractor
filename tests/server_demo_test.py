#!/usr/bin/env python3
"""End-to-end test of the demo endpoints (run by ctest; standard library only).

usage: server_demo_test.py <fpx-server binary> <work dir>
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

SERVER, WORK = sys.argv[1], sys.argv[2]
os.makedirs(WORK, exist_ok=True)
DB = os.path.join(WORK, "demo.fpxdb")
if os.path.exists(DB):
    os.remove(DB)
failures = []


def check(ok, name):
    print(("ok   " if ok else "FAIL ") + name)
    if not ok:
        failures.append(name)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start(*extra):
    port = free_port()
    proc = subprocess.Popen([SERVER, "--db", DB, "--host", "127.0.0.1", "--port", str(port), *extra],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    url = f"http://127.0.0.1:{port}"
    for _ in range(100):
        try:
            urllib.request.urlopen(url + "/healthz", timeout=1)
            return proc, url
        except Exception:
            time.sleep(0.1)
    proc.kill()
    raise SystemExit("server did not start")


def call(url, path, body):
    req = urllib.request.Request(url + path, data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        raw = e.read()
        return e.code, json.loads(raw) if raw else {}


proc, url = start()
try:
    st, _ = call(url, "/v1/demo/synthetic", {"finger_id": 1, "impression": 0})
    check(st == 404, "synthetic generation is off without --demo")
finally:
    proc.terminate()
    proc.wait(timeout=10)

proc, url = start("--demo")
try:
    def synth(finger, impression):
        st, out = call(url, "/v1/demo/synthetic", {"finger_id": finger, "impression": impression})
        assert st == 200, out
        return out["file"]

    a0, a1, b0 = synth(11, 0), synth(11, 1), synth(22, 0)
    again = synth(11, 0)
    check(a0 == again, "synthetic generation is reproducible")

    st, ins = call(url, "/v1/inspect", {"file": a0})
    check(st == 200 and ins["kind"] == "image" and ins["source_format"] == "BMP", "inspect recognises a BMP")
    check([s["name"] for s in ins["steps"]] == ["decode", "wsq", "minex"], "inspect reports BMP -> WSQ -> MINEX")
    check(len(ins["minutiae"]) >= 6, f"inspect returns minutiae ({len(ins['minutiae'])})")
    m = ins["minutiae"][0]
    check(all(k in m for k in ("x", "y", "angle", "type", "quality")), "minutiae have coordinates and angles")
    check(ins["preview_bmp"][:2] == "Qk", "inspect returns a browser-viewable preview")
    ansi, iso, wsq = ins["template_ansi"], ins["template_iso"], ins["wsq"]
    check(len(base64.b64decode(wsq)) < len(base64.b64decode(a0)), "WSQ is smaller than the BMP")

    st, ins_wsq = call(url, "/v1/inspect", {"file": wsq})
    check(st == 200 and ins_wsq["source_format"] == "WSQ" and ins_wsq["wsq"] == wsq, "inspect accepts WSQ unchanged")
    st, ins_ansi = call(url, "/v1/inspect", {"file": ansi})
    check(st == 200 and ins_ansi["kind"] == "template" and "ANSI" in ins_ansi["source_format"], "inspect accepts MINEX")
    check("preview_bmp" not in ins_ansi and len(ins_ansi["minutiae"]) == len(ins["minutiae"]),
          "a template has minutiae but no image")
    st, ins_iso = call(url, "/v1/inspect", {"file": iso})
    check(st == 200 and "ISO" in ins_iso["source_format"], "inspect accepts ISO")

    st, bad = call(url, "/v1/inspect", {"file": base64.b64encode(b"not a fingerprint").decode()})
    check(st == 400 and "error" in bad, "garbage upload is a clean 400")
    st, bad = call(url, "/v1/inspect", {})
    check(st == 400, "missing file is a clean 400")

    st, genuine = call(url, "/v1/compare", {"a": a0, "b": a1})
    check(st == 200 and genuine["match"] and genuine["score"] >= genuine["threshold"], "same finger matches")
    check(len(genuine["pairs"]) >= 3, f"compare returns matched pairs ({len(genuine['pairs'])})")
    xy = {(p["x"], p["y"]) for p in ins["minutiae"]}
    check(all((p["a"]["x"], p["a"]["y"]) in xy for p in genuine["pairs"]), "pair coordinates are real minutiae of a")
    st, impostor = call(url, "/v1/compare", {"a": a0, "b": b0})
    check(st == 200 and not impostor["match"], "different fingers do not match")
    st, mixed = call(url, "/v1/compare", {"a": ansi, "b": a1})
    check(st == 200 and mixed["match"], "MINEX compares against a BMP")
    st, mixed = call(url, "/v1/compare", {"a": ansi, "b": iso})
    check(st == 200 and mixed["score"] > 90, "MINEX and ISO of one scan agree")

    files = [{"name": "a0", "file": a0}, {"name": "a1", "file": a1}, {"name": "b0", "file": b0}]
    st, cm = call(url, "/v1/crossmatch", {"files": files})
    check(st == 200 and cm["names"] == ["a0", "a1", "b0"], "cross-match keeps file order")
    s = cm["scores"]
    check(s[0][1] == s[1][0] and s[0][2] == s[2][0], "cross-match is symmetric")
    check(s[0][1] >= cm["threshold"] > s[0][2], "cross-match separates genuine from impostor")
    st, bad = call(url, "/v1/crossmatch", {"files": files[:1] * 101})
    check(st == 400, "oversized cross-match is rejected")
    st, cm = call(url, "/v1/crossmatch", {"files": [files[0], {"name": "junk", "file": "AAAA"}]})
    check(st == 400 and "junk" in cm["error"], "cross-match error names the bad file")
finally:
    proc.terminate()
    proc.wait(timeout=10)

print(f"\n{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
