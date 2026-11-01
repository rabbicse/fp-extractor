#!/usr/bin/env python3
"""Device-side example: bitmap -> WSQ + ANSI-378 ("MINEX") template via libfpx (ctypes).

Usage:
    python3 examples/python/fpx_ctypes.py build/libfpx.so finger.bmp [other.bmp]

This is what runs next to the scanner: whatever SDK captured the image, save it
as BMP (or hand over raw pixels) and convert it here. Only the template (~0.3-1 KB)
has to travel to the server.
"""
import ctypes
import sys

FPX_FORMAT_ANSI378 = 1
FPX_FORMAT_ISO19794 = 2


class Fpx:
    def __init__(self, lib_path):
        self.lib = ctypes.CDLL(lib_path)
        u8p = ctypes.POINTER(ctypes.c_ubyte)
        L = self.lib
        L.fpx_version.restype = ctypes.c_char_p
        L.fpx_last_error.restype = ctypes.c_char_p
        L.fpx_default_threshold.restype = ctypes.c_float
        L.fpx_free.argtypes = [ctypes.c_void_p]
        L.fpx_extract_from_image.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int,
                                             ctypes.POINTER(u8p), ctypes.POINTER(ctypes.c_size_t)]
        L.fpx_extract_from_raw.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                           ctypes.POINTER(u8p), ctypes.POINTER(ctypes.c_size_t)]
        L.fpx_image_to_wsq.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_float,
                                       ctypes.POINTER(u8p), ctypes.POINTER(ctypes.c_size_t)]
        L.fpx_match.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t,
                                ctypes.POINTER(ctypes.c_float)]

    def _call(self, fn, *args):
        out = ctypes.POINTER(ctypes.c_ubyte)()
        n = ctypes.c_size_t()
        rc = fn(*args, ctypes.byref(out), ctypes.byref(n))
        if rc != 0:
            raise RuntimeError(f"fpx error {rc}: {self.lib.fpx_last_error().decode()}")
        try:
            return ctypes.string_at(out, n.value)
        finally:
            self.lib.fpx_free(out)

    def version(self):
        return self.lib.fpx_version().decode()

    def extract(self, image_bytes, dpi=500, fmt=FPX_FORMAT_ANSI378):
        return self._call(self.lib.fpx_extract_from_image, image_bytes, len(image_bytes), dpi, fmt)

    def extract_raw(self, pixels, width, height, dpi=500, fmt=FPX_FORMAT_ANSI378):
        return self._call(self.lib.fpx_extract_from_raw, pixels, width, height, dpi, fmt)

    def to_wsq(self, image_bytes, dpi=500, bitrate=0.75):
        return self._call(self.lib.fpx_image_to_wsq, image_bytes, len(image_bytes), dpi, bitrate)

    def match(self, a, b):
        s = ctypes.c_float()
        rc = self.lib.fpx_match(a, len(a), b, len(b), ctypes.byref(s))
        if rc != 0:
            raise RuntimeError(self.lib.fpx_last_error().decode())
        return s.value

    def threshold(self):
        return self.lib.fpx_default_threshold()


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    fpx = Fpx(sys.argv[1])
    print("libfpx", fpx.version())
    img = open(sys.argv[2], "rb").read()
    wsq = fpx.to_wsq(img)
    tmpl = fpx.extract(img)
    print(f"{sys.argv[2]}: {len(img)} B image -> {len(wsq)} B WSQ, {len(tmpl)} B ANSI-378 template")
    if len(sys.argv) > 3:
        other = fpx.extract(open(sys.argv[3], "rb").read(), fmt=FPX_FORMAT_ISO19794)
        s = fpx.match(tmpl, other)
        print(f"score vs {sys.argv[3]}: {s:.1f} -> {'MATCH' if s >= fpx.threshold() else 'no match'}")
