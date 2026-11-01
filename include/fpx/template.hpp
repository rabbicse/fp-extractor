// SPDX-License-Identifier: MIT
// Fingerprint Minutiae Record codec:
//   * ANSI/INCITS 378-2004  ("MINEX" format)
//   * ISO/IEC 19794-2:2005  (record format)
// Both are what FingerJet (and most scanner SDKs) emit, so the server only ever
// needs these bytes - never the image - which keeps the solution device-agnostic.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fpx {

enum class TemplateFormat : uint8_t { Ansi378_2004 = 1, Iso19794_2_2005 = 2 };

const char* to_string(TemplateFormat f);
/// Parse "ansi", "minex", "ansi378", "iso", "iso19794" (case-insensitive).
TemplateFormat parse_template_format(const std::string& s);

enum class MinutiaType : uint8_t { Other = 0, Ending = 1, Bifurcation = 2 };

struct Minutia {
    uint16_t x = 0;        ///< pixels, origin top-left
    uint16_t y = 0;        ///< pixels, y grows downward
    uint8_t angle = 0;     ///< direction in 1/256 of a full turn, counter-clockwise (ISO units)
    MinutiaType type = MinutiaType::Other;
    uint8_t quality = 0;   ///< 0..100
};

struct FingerTemplate {
    TemplateFormat format = TemplateFormat::Iso19794_2_2005;
    uint16_t width = 0;            ///< image size in pixels
    uint16_t height = 0;
    uint16_t xres = 197;           ///< pixels per centimetre (197 ≈ 500 dpi)
    uint16_t yres = 197;
    uint32_t cbeff_product_id = 0; ///< ANSI only
    uint8_t finger_position = 0;   ///< 0 = unknown, 1 = right thumb ... 10 = left little
    uint8_t impression_type = 0;   ///< 0 = live-scan plain
    uint8_t finger_quality = 0;    ///< 0..100
    std::vector<Minutia> minutiae;
};

/// Detect ANSI vs ISO from the header (both start with "FMR\0 20\0").
TemplateFormat detect_template_format(const std::vector<uint8_t>& bytes);

/// Parse an ANSI-378 or ISO-19794-2 record (auto-detected). Only the first
/// finger view is used. Throws fpx::Error on malformed input.
FingerTemplate parse_template(const std::vector<uint8_t>& bytes);
FingerTemplate parse_template(const uint8_t* data, size_t size);

/// Serialise to ANSI-378-2004 or ISO-19794-2-2005 (one view, no extended data).
std::vector<uint8_t> serialize_template(const FingerTemplate& t, TemplateFormat format);

/// True if the bytes look like a minutiae record (cheap header check only).
bool looks_like_template(const std::vector<uint8_t>& bytes);

}  // namespace fpx
