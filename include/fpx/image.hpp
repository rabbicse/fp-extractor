// SPDX-License-Identifier: MIT
// Grayscale fingerprint image + decoders/encoders for the formats scanners
// actually produce: BMP (8/24/32 bit), PGM, uncompressed TIFF, raw, WSQ.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace fpx {

/// Thrown for malformed inputs (bad image, bad template, ...).
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// 8-bit grayscale image, row-major, top-down, no row padding.
/// Dark ridges on light background, as expected by FingerJet and ISO 19794-4.
struct GrayImage {
    int width = 0;
    int height = 0;
    int dpi = 500;                 ///< pixels per inch (square pixels)
    std::vector<uint8_t> pixels;   ///< size == width * height

    GrayImage() = default;
    GrayImage(int w, int h, int dpi_ = 500, uint8_t fill = 255)
        : width(w), height(h), dpi(dpi_), pixels(static_cast<size_t>(w) * h, fill) {}

    uint8_t& at(int x, int y) { return pixels[static_cast<size_t>(y) * width + x]; }
    uint8_t at(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
    bool empty() const { return pixels.empty(); }
};

enum class ImageFormat { Unknown, Bmp, Pgm, Tiff, Wsq };

ImageFormat detect_image_format(const std::vector<uint8_t>& bytes);
ImageFormat image_format_from_extension(const std::string& path);
const char* to_string(ImageFormat f);

/// Decode any supported format (auto-detected from magic bytes).
/// `default_dpi` is used when the file carries no usable resolution.
GrayImage decode_image(const std::vector<uint8_t>& bytes, int default_dpi = 500);

GrayImage decode_bmp(const std::vector<uint8_t>& bytes, int default_dpi = 500);
GrayImage decode_pgm(const std::vector<uint8_t>& bytes, int default_dpi = 500);
GrayImage decode_tiff(const std::vector<uint8_t>& bytes, int default_dpi = 500);
/// Raw 8-bit pixels with no header (what many scanner SDKs hand you).
GrayImage from_raw(const uint8_t* data, size_t size, int width, int height, int dpi = 500);

std::vector<uint8_t> encode_bmp(const GrayImage& img);  ///< 8-bit palettised BMP
std::vector<uint8_t> encode_pgm(const GrayImage& img);  ///< binary P5 PGM

/// Encode according to `fmt` (Bmp, Pgm or Wsq).
std::vector<uint8_t> encode_image(const GrayImage& img, ImageFormat fmt, float wsq_bitrate = 0.75f);

// File helpers -------------------------------------------------------------
std::vector<uint8_t> read_file(const std::string& path);
void write_file(const std::string& path, const std::vector<uint8_t>& bytes);
GrayImage load_image(const std::string& path, int default_dpi = 500);
/// Format chosen from the extension (.bmp, .pgm, .wsq).
void save_image(const std::string& path, const GrayImage& img, float wsq_bitrate = 0.75f);

// Simple geometric helpers -------------------------------------------------
/// Crop (or pad with white) to w x h, keeping the image centred.
GrayImage fit_center(const GrayImage& img, int w, int h);

}  // namespace fpx
