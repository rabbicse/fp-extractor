// SPDX-License-Identifier: MIT
#include "fpx/extractor.hpp"

#include <algorithm>

#include "FRFXLL.h"

namespace fpx {
namespace {

constexpr uint32_t kCbeffDigitalPersona = 0x00330502;  // FingerJet product id (CBEFF registry)

struct Handle {
    FRFXLL_HANDLE h = nullptr;
    ~Handle() {
        if (h) FRFXLLCloseHandle(&h);
    }
};

std::string frfxll_error(FRFXLL_RESULT rc) {
    switch (rc) {
        case FRFXLL_ERR_FB_TOO_SMALL_AREA: return "fingerprint area too small / no fingerprint detected";
        case FRFXLL_ERR_INVALID_IMAGE: return "invalid image";
        case FRFXLL_ERR_INVALID_PARAM: return "invalid parameter";
        case FRFXLL_ERR_NO_MEMORY: return "out of memory";
        case FRFXLL_ERR_MORE_DATA: return "output buffer too small";
        default: return "FingerJet error " + std::to_string(rc);
    }
}

// Centre of the fingerprint: centroid of high-variance 16x16 blocks (ridges),
// so the analysis window lands on the finger even on large-platen scanners.
void foreground_center(const GrayImage& img, int& cx, int& cy) {
    const int B = 16;
    double sx = 0, sy = 0, sw = 0;
    for (int by = 0; by + B <= img.height; by += B)
        for (int bx = 0; bx + B <= img.width; bx += B) {
            double s = 0, s2 = 0;
            for (int y = by; y < by + B; ++y)
                for (int x = bx; x < bx + B; ++x) {
                    double v = img.at(x, y);
                    s += v;
                    s2 += v * v;
                }
            const double n = B * B, var = s2 / n - (s / n) * (s / n);
            if (var > 300.0) {  // ridge/valley contrast; background is flat
                sx += (bx + B / 2) * var;
                sy += (by + B / 2) * var;
                sw += var;
            }
        }
    if (sw > 0) {
        cx = int(sx / sw);
        cy = int(sy / sw);
    }
}

GrayImage crop(const GrayImage& in, int ox, int oy, int w, int h) {
    GrayImage out(w, h, in.dpi, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = x + ox, sy = y + oy;
            if (sx >= 0 && sy >= 0 && sx < in.width && sy < in.height) out.at(x, y) = in.at(sx, sy);
        }
    return out;
}

}  // namespace

ExtractResult extract(const GrayImage& input, const ExtractOptions& opt) {
    if (input.empty()) throw Error("extract: empty image");
    if (input.dpi < 300 || input.dpi > 1024)
        throw Error("extract: resolution " + std::to_string(input.dpi) +
                    " dpi not supported (300..1024). Pass the real sensor dpi (--dpi).");

    // FingerJet only analyses a window of 256 x 400 px at its internal 333 dpi
    // (~0.77" x 1.2", i.e. 384 x 600 px at 500 dpi) and silently centre-crops
    // larger images. We crop ourselves - around the fingerprint, not the image
    // centre - and map minutiae back into original image coordinates.
    const int dpi = input.dpi;
    const int min_px = (150 * dpi + 499) / 500;            // 0.3"
    const int win_w = 256 * dpi / 333 - 4, win_h = 400 * dpi / 333 - 4;
    const GrayImage* img = &input;
    GrayImage work;
    int ox = 0, oy = 0;
    const bool too_small = input.width < min_px || input.height < min_px;
    const bool too_big = input.width > win_w || input.height > win_h;
    if (too_small || too_big) {
        if (!opt.auto_fit)
            throw Error("extract: image " + std::to_string(input.width) + "x" + std::to_string(input.height) +
                        " outside FingerJet's analysis window at " + std::to_string(dpi) + " dpi (max " +
                        std::to_string(win_w) + "x" + std::to_string(win_h) + ")");
        const int w = std::clamp(input.width, min_px, win_w), h = std::clamp(input.height, min_px, win_h);
        int cx = input.width / 2, cy = input.height / 2;
        if (too_big) foreground_center(input, cx, cy);
        ox = std::clamp(cx - w / 2, std::min(0, input.width - w), std::max(0, input.width - w));
        oy = std::clamp(cy - h / 2, std::min(0, input.height - h), std::max(0, input.height - h));
        work = crop(input, ox, oy, w, h);
        img = &work;
    }

    Handle ctx, fs;
    FRFXLL_RESULT rc = FRFXLLCreateLibraryContext(&ctx.h);
    if (rc < FRFXLL_OK) throw Error("extract: cannot create FingerJet context: " + frfxll_error(rc));
    rc = FRFXLLCreateFeatureSetFromRaw(ctx.h, img->pixels.data(), img->pixels.size(), unsigned(img->width),
                                       unsigned(img->height), unsigned(dpi), FRFXLL_FEX_ENABLE_ENHANCEMENT, &fs.h);
    if (rc < FRFXLL_OK) throw Error("extract: " + frfxll_error(rc));

    const unsigned short dpcm = static_cast<unsigned short>((dpi * 100 + 50) / 254);
    FRFXLL_OUTPUT_PARAM_ISO_ANSI param = {sizeof(FRFXLL_OUTPUT_PARAM_ISO_ANSI),
                                          kCbeffDigitalPersona,
                                          opt.finger_position,
                                          0,
                                          dpcm,
                                          dpcm,
                                          static_cast<unsigned short>(img->width),
                                          static_cast<unsigned short>(img->height),
                                          0,
                                          60,  // finger quality (FingerJet has no NFIQ; 60 ~ NFIQ 3)
                                          opt.impression_type};
    const FRFXLL_DATA_TYPE dt =
        opt.format == TemplateFormat::Ansi378_2004 ? FRFXLL_DT_ANSI_FEATURE_SET : FRFXLL_DT_ISO_FEATURE_SET;

    ExtractResult r;
    r.record.resize(34 + 256 * 6);
    size_t size = r.record.size();
    // FRFXLLExport is marked deprecated upstream only because newer ISO/ANSI
    // revisions are out of scope for FingerJet; it is the supported 2004/2005 path.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    rc = FRFXLLExport(fs.h, dt, &param, r.record.data(), &size);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    if (rc < FRFXLL_OK) throw Error("extract: export failed: " + frfxll_error(rc));
    r.record.resize(size);
    r.parsed = parse_template(r.record);
    if (img != &input) {
        // Express minutiae in the coordinates of the image the caller gave us.
        for (auto& m : r.parsed.minutiae) {
            m.x = uint16_t(std::clamp(int(m.x) + ox, 0, 0x3FFF));
            m.y = uint16_t(std::clamp(int(m.y) + oy, 0, 0x3FFF));
        }
        r.parsed.width = uint16_t(input.width);
        r.parsed.height = uint16_t(input.height);
        const uint32_t cbeff = r.parsed.cbeff_product_id;
        r.record = serialize_template(r.parsed, opt.format);
        r.parsed = parse_template(r.record);
        r.parsed.cbeff_product_id = cbeff;
    }
    return r;
}

std::vector<uint8_t> extract_template(const GrayImage& img, const ExtractOptions& opt) {
    return extract(img, opt).record;
}

}  // namespace fpx
