// SPDX-License-Identifier: MIT
// FBI WSQ (Wavelet Scalar Quantization) compression, backed by NIST NBIS.
#pragma once

#include <cstdint>
#include <vector>

#include "fpx/image.hpp"

namespace fpx {

/// Compress a grayscale image to WSQ.
/// `bitrate` 0.75 ≈ 15:1 (FBI recommendation for 500 ppi), 2.25 ≈ 5:1.
std::vector<uint8_t> wsq_encode(const GrayImage& img, float bitrate = 0.75f);

/// Decompress a WSQ file. Resolution is taken from the file (NISTCOM PPI) if present.
GrayImage wsq_decode(const std::vector<uint8_t>& wsq, int default_dpi = 500);

}  // namespace fpx
