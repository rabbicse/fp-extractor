// Device-side example in Go: bitmap -> WSQ + ANSI-378 ("MINEX") template, and a
// 1:1 match, by calling libfpx through cgo. Only the template (~0.5 KB) needs to
// travel to the server; see examples/go-gateway for the server side in Go.
//
//	cmake --build build                      # builds build/libfpx.so
//	cd examples/go-cgo
//	CGO_CFLAGS="-I$PWD/../../include" CGO_LDFLAGS="-L$PWD/../../build -Wl,-rpath,$PWD/../../build" \
//	    go run . ../../fpx-demo/finger1_0.bmp ../../fpx-demo/finger1_1.bmp
package main

/*
#cgo LDFLAGS: -lfpx
#include <stdlib.h>
#include "fpx/fpx_c.h"
*/
import "C"

import (
	"errors"
	"fmt"
	"os"
	"unsafe"
)

// Formats from fpx_c.h.
const (
	FormatANSI378 = C.FPX_FORMAT_ANSI378
	FormatISO     = C.FPX_FORMAT_ISO19794
)

func lastError(rc C.int) error {
	return fmt.Errorf("libfpx error %d: %s", int(rc), C.GoString(C.fpx_last_error()))
}

// cBytes passes a Go slice to C without copying (valid for the duration of the call).
func cBytes(b []byte) (*C.uchar, C.size_t) {
	if len(b) == 0 {
		return nil, 0
	}
	return (*C.uchar)(unsafe.Pointer(&b[0])), C.size_t(len(b))
}

// takeBuffer copies a libfpx-owned buffer into Go memory and frees it.
func takeBuffer(p *C.uchar, n C.size_t) []byte {
	defer C.fpx_free(unsafe.Pointer(p))
	return C.GoBytes(unsafe.Pointer(p), C.int(n))
}

// Extract turns image bytes (BMP/PGM/TIFF/WSQ) into a template. dpi 0 = from the file, else 500.
func Extract(image []byte, dpi int, format int) ([]byte, error) {
	img, n := cBytes(image)
	var out *C.uchar
	var outLen C.size_t
	if rc := C.fpx_extract_from_image(img, n, C.int(dpi), C.int(format), &out, &outLen); rc != C.FPX_OK {
		return nil, lastError(rc)
	}
	return takeBuffer(out, outLen), nil
}

// ToWSQ compresses image bytes to WSQ (bitrate 0.75 is the usual choice at 500 dpi).
func ToWSQ(image []byte, dpi int, bitrate float32) ([]byte, error) {
	img, n := cBytes(image)
	var out *C.uchar
	var outLen C.size_t
	if rc := C.fpx_image_to_wsq(img, n, C.int(dpi), C.float(bitrate), &out, &outLen); rc != C.FPX_OK {
		return nil, lastError(rc)
	}
	return takeBuffer(out, outLen), nil
}

// Match compares two templates (ANSI or ISO) and returns a score in [0, 100].
func Match(a, b []byte) (float32, error) {
	if len(a) == 0 || len(b) == 0 {
		return 0, errors.New("empty template")
	}
	pa, na := cBytes(a)
	pb, nb := cBytes(b)
	var score C.float
	if rc := C.fpx_match(pa, na, pb, nb, &score); rc != C.FPX_OK {
		return 0, lastError(rc)
	}
	return float32(score), nil
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: go run . finger.bmp [other.bmp]")
		os.Exit(2)
	}
	fmt.Println("libfpx", C.GoString(C.fpx_version()))
	var templates [][]byte
	for _, path := range os.Args[1:] {
		image, err := os.ReadFile(path)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		wsq, err := ToWSQ(image, 0, 0.75)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		tmpl, err := Extract(image, 0, FormatANSI378)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		fmt.Printf("%s: %d B image -> %d B WSQ, %d B ANSI-378 template\n", path, len(image), len(wsq), len(tmpl))
		templates = append(templates, tmpl)
	}
	if len(templates) == 2 {
		score, err := Match(templates[0], templates[1])
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		decision := "NO MATCH"
		if score >= float32(C.fpx_default_threshold()) {
			decision = "MATCH"
		}
		fmt.Printf("score %.1f (threshold %.0f) -> %s\n", score, float32(C.fpx_default_threshold()), decision)
	}
}
