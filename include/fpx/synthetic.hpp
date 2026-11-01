// SPDX-License-Identifier: MIT
// Synthetic fingerprints for tests, demos and benchmarks - no real biometric
// data needs to be committed to the repository.
//
//  * synthesize_image():   ridge pattern grown by iterative oriented Gabor
//    filtering over a loop/whorl/arch orientation model (SFinGe-style, much
//    simplified). Several "impressions" of the same finger share the master
//    pattern but differ in placement, rotation, distortion and noise, so the
//    real FingerJet extractor + matcher can be tested end-to-end.
//  * synthesize_template() / perturb_template(): minutiae-level generator used
//    to build million-template galleries quickly for performance tests.
#pragma once

#include <cstdint>

#include "fpx/image.hpp"
#include "fpx/template.hpp"

namespace fpx {

struct SynthImageOptions {
    int width = 320;          ///< sensor size (pixels)
    int height = 400;
    int dpi = 500;
    float rotation_deg = 0;   ///< impression placement
    int dx = 0, dy = 0;
    float distortion = 0;     ///< 0..1 non-linear skin distortion amount
    float noise = 0.08f;      ///< 0..1 sensor noise
    /// If true, rotation/offset/distortion/noise are drawn randomly from the
    /// impression number (impression 0 is always the clean, centred one).
    bool random_placement = true;
};

/// Deterministic for a given (finger_id, impression, options).
GrayImage synthesize_image(uint32_t finger_id, uint32_t impression, const SynthImageOptions& opt = {});

struct SynthTemplateOptions {
    int width = 320, height = 400;
    int min_minutiae = 25, max_minutiae = 55;
};

/// Random but fingerprint-like (orientation-field-consistent) minutiae set.
FingerTemplate synthesize_template(uint64_t finger_id, const SynthTemplateOptions& opt = {});

struct PerturbOptions {
    float max_rotation_deg = 25;
    int max_shift = 40;
    float position_jitter = 3.0f;   ///< px (std-dev)
    int angle_jitter = 6;           ///< 1/256 turn (std-dev)
    float drop_rate = 0.25f;        ///< fraction of minutiae missed
    float spurious_rate = 0.15f;    ///< fraction of false minutiae added
};

/// Simulate another impression of the same finger at minutiae level.
FingerTemplate perturb_template(const FingerTemplate& t, uint64_t seed, const PerturbOptions& opt = {});

}  // namespace fpx
