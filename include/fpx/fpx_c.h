/* SPDX-License-Identifier: MIT
 *
 * Plain C ABI for libfpx - use from C, Python (ctypes/cffi), C# (P/Invoke),
 * Java (JNA), Go (cgo), Node (ffi-napi), ...
 *
 * All functions return 0 on success, a negative FPX_E_* code on failure.
 * fpx_last_error() returns a thread-local, human-readable message.
 * Buffers returned through `unsigned char** out` must be released with fpx_free().
 */
#ifndef FPX_C_H
#define FPX_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define FPX_API __declspec(dllexport)
#else
#define FPX_API __attribute__((visibility("default")))
#endif

#define FPX_OK 0
#define FPX_E_INVALID_ARG (-1)
#define FPX_E_BAD_INPUT (-2)      /* undecodable image / template */
#define FPX_E_EXTRACTION (-3)     /* no fingerprint found, etc. */
#define FPX_E_NOT_FOUND (-4)
#define FPX_E_INTERNAL (-99)

#define FPX_FORMAT_ANSI378 1      /* ANSI INCITS 378-2004 ("MINEX") */
#define FPX_FORMAT_ISO19794 2     /* ISO/IEC 19794-2:2005 */

FPX_API const char* fpx_version(void);
FPX_API const char* fpx_last_error(void);
FPX_API void fpx_free(void* p);

/* Image bytes (BMP/PGM/TIFF/WSQ, auto-detected) -> minutiae template.
 * dpi: used when the file carries no resolution (0 -> 500). */
FPX_API int fpx_extract_from_image(const unsigned char* image, size_t image_len, int dpi, int format,
                                   unsigned char** out_template, size_t* out_len);

/* Raw 8-bit grayscale pixels (top-down, no padding) -> template. */
FPX_API int fpx_extract_from_raw(const unsigned char* pixels, int width, int height, int dpi, int format,
                                 unsigned char** out_template, size_t* out_len);

/* Image bytes -> WSQ bytes. bitrate 0.75 recommended for 500 dpi. */
FPX_API int fpx_image_to_wsq(const unsigned char* image, size_t image_len, int dpi, float bitrate,
                             unsigned char** out_wsq, size_t* out_len);

/* Convert a template between ANSI-378 and ISO-19794-2. */
FPX_API int fpx_convert_template(const unsigned char* tmpl, size_t len, int format, unsigned char** out,
                                 size_t* out_len);

/* 1:1 comparison of two templates (ANSI or ISO). Score in [0,100]. */
FPX_API int fpx_match(const unsigned char* a, size_t a_len, const unsigned char* b, size_t b_len, float* score);

/* Recommended decision threshold for fpx_match scores. */
FPX_API float fpx_default_threshold(void);

#ifdef __cplusplus
}
#endif

#endif /* FPX_C_H */
