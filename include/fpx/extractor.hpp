// SPDX-License-Identifier: MIT
// Minutiae extraction (bitmap -> ANSI 378 / ISO 19794-2) using FingerJet FX OSE.
#pragma once

#include <cstdint>
#include <vector>

#include "fpx/image.hpp"
#include "fpx/template.hpp"

namespace fpx {

struct ExtractOptions {
    TemplateFormat format = TemplateFormat::Iso19794_2_2005;
    uint8_t finger_position = 0;  ///< 0 unknown, 1..10 per ISO 19794-2
    uint8_t impression_type = 0;  ///< 0 live-scan plain
    /// FingerJet analyses at most ~0.77" x 1.2" (384 x 600 px at 500 dpi) and
    /// needs at least 0.3". Larger images are cropped around the detected
    /// fingerprint (minutiae are reported in original image coordinates);
    /// smaller ones are padded. With auto_fit = false such images throw.
    bool auto_fit = true;
};

struct ExtractResult {
    std::vector<uint8_t> record;  ///< the ANSI/ISO bytes, ready to store/send
    FingerTemplate parsed;        ///< same data, decoded
};

/// Extract minutiae from a grayscale image. Throws fpx::Error on failure
/// (e.g. no fingerprint found, unsupported resolution).
ExtractResult extract(const GrayImage& img, const ExtractOptions& opt = {});

/// Convenience: just the record bytes.
std::vector<uint8_t> extract_template(const GrayImage& img, const ExtractOptions& opt = {});

}  // namespace fpx
