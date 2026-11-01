// Minimal end-to-end example of the C++ API:
//   scanner bitmap -> WSQ (archive) -> ISO/ANSI template -> enrol -> identify
// Build: part of the CMake project (target example_pipeline).
// Run:   ./example_pipeline finger.bmp [probe.bmp]
#include <cstdio>

#include "fpx/fpx.hpp"

int main(int argc, char** argv) {
    try {
        // 1. Load the bitmap exactly as the scanner SDK saved it (BMP/PGM/TIFF/WSQ).
        //    No scanner? use a synthetic print instead.
        fpx::GrayImage img = argc > 1 ? fpx::load_image(argv[1]) : fpx::synthesize_image(42, 0);
        std::printf("image: %dx%d @ %d dpi\n", img.width, img.height, img.dpi);

        // 2. WSQ for archival / regulatory storage of the raw image.
        std::vector<uint8_t> wsq = fpx::wsq_encode(img, 0.75f);
        std::printf("wsq: %zu bytes (%.1fx smaller)\n", wsq.size(), double(img.pixels.size()) / wsq.size());

        // 3. Minutiae template - this is all the server ever needs.
        fpx::ExtractOptions opt;
        opt.format = fpx::TemplateFormat::Ansi378_2004;  // "MINEX" format; or Iso19794_2_2005
        fpx::ExtractResult enrolled = fpx::extract(img, opt);
        std::printf("template: %zu bytes, %zu minutiae\n", enrolled.record.size(), enrolled.parsed.minutiae.size());

        // 4. Enrol and search.
        fpx::Gallery gallery;
        gallery.insert("customer-42", enrolled.parsed);
        for (uint32_t f = 1; f <= 5; ++f)  // a few other people
            gallery.insert("customer-other-" + std::to_string(f), fpx::extract(fpx::synthesize_image(1000 + f, 0)).parsed);

        fpx::GrayImage probe_img = argc > 2 ? fpx::load_image(argv[2]) : fpx::synthesize_image(42, 1);
        fpx::FingerTemplate probe = fpx::extract(probe_img).parsed;
        fpx::IdentifyResult r = gallery.identify(probe, {3, 0, 0});
        for (auto& c : r.candidates)
            std::printf("candidate %-18s score %6.2f %s\n", c.id.c_str(), c.score,
                        c.score >= fpx::kIdentifyThreshold ? "MATCH" : "");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
