#!/usr/bin/env python3
"""Minimal REST client for fpx-server / the Go gateway (standard library only).

Fingers are given as FILE[:POSITION]; position 1..10 per ISO 19794-2
(1 right thumb, 2 right index, 3 right middle, ... 6 left thumb, 7 left index, ...).

    python3 client.py enroll   cust-1 right_index.iso:2 left_index.bmp:7
    python3 client.py identify probe_ri.ansi:2 probe_li.bmp:7
    python3 client.py verify   cust-1 probe_ri.iso:2 probe_li.iso:7
    python3 client.py add-finger cust-1 right_middle.bmp 3
    python3 client.py get      cust-1
    python3 client.py delete   cust-1
    FPX_URL=http://host:8080 FPX_API_KEY=secret python3 client.py stats

Files starting with "FMR\\0" (ANSI-378 / ISO-19794-2) are sent as templates,
anything else (BMP/PGM/TIFF/WSQ) as images for server-side extraction.
"""
import base64
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

URL = os.environ.get("FPX_URL", "http://localhost:8080")
KEY = os.environ.get("FPX_API_KEY", "")


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(URL + path, data=data, method=method,
                                 headers={"Content-Type": "application/json", "X-API-Key": KEY})
    try:
        with urllib.request.urlopen(req) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        return {"http_status": e.code, **json.load(e)}


def finger(arg):
    path, pos = arg, None
    head, sep, tail = arg.rpartition(":")
    if sep and tail.isdigit():
        path, pos = head, int(tail)
    raw = open(path, "rb").read()
    item = {"template" if raw[:4] == b"FMR\x00" else "image": base64.b64encode(raw).decode()}
    if pos is not None:
        item["position"] = pos
    return item


def sid(s):
    return urllib.parse.quote(s, safe="")


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    cmd = a[0]
    if cmd == "enroll" and len(a) >= 3:
        out = call("PUT", f"/v1/subjects/{sid(a[1])}", {"fingers": [finger(x) for x in a[2:]]})
    elif cmd == "add-finger" and len(a) == 4:
        out = call("PUT", f"/v1/subjects/{sid(a[1])}/fingers/{int(a[3])}", finger(a[2]))
    elif cmd == "identify" and len(a) >= 2:
        out = call("POST", "/v1/identify", {"fingers": [finger(x) for x in a[1:]], "top_k": 5})
    elif cmd == "verify" and len(a) >= 3:
        out = call("POST", "/v1/verify", {"id": a[1], "fingers": [finger(x) for x in a[2:]]})
    elif cmd == "get" and len(a) == 2:
        out = call("GET", f"/v1/subjects/{sid(a[1])}")
    elif cmd == "delete" and len(a) == 2:
        out = call("DELETE", f"/v1/subjects/{sid(a[1])}")
    elif cmd == "stats":
        out = call("GET", "/v1/stats")
    else:
        sys.exit(__doc__)
    print(json.dumps(out, indent=2))
