#!/usr/bin/env python3
"""Minimal REST client for fpx-server (standard library only).

    python3 examples/python/client.py enroll   customer-1 finger.iso
    python3 examples/python/client.py identify probe.ansi
    python3 examples/python/client.py verify   customer-1 probe.bmp
    FPX_URL=http://host:8080 FPX_API_KEY=secret python3 examples/python/client.py stats

Files ending in .iso/.ansi/.ist/.min/.tmpl are sent as templates, others as images.
"""
import base64
import json
import os
import sys
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


def payload(path):
    raw = open(path, "rb").read()
    b64 = base64.b64encode(raw).decode()
    is_template = raw[:4] == b"FMR\x00"
    return {"template": b64} if is_template else {"image": b64}


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    if a[0] == "enroll":
        out = call("PUT", f"/v1/subjects/{a[1]}", payload(a[2]))
    elif a[0] == "identify":
        out = call("POST", "/v1/identify", {**payload(a[1]), "top_k": 5})
    elif a[0] == "verify":
        out = call("POST", "/v1/verify", {"id": a[1], **payload(a[2])})
    elif a[0] == "delete":
        out = call("DELETE", f"/v1/subjects/{a[1]}")
    elif a[0] == "stats":
        out = call("GET", "/v1/stats")
    else:
        sys.exit(__doc__)
    print(json.dumps(out, indent=2))
