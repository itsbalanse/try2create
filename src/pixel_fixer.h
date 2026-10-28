#pragma once
// A C++ port of Retro Diffusion's Pixel Art Fixer (https://github.com/Retro-Diffusion/pixel-art-fixer, MIT licence, Copyright (c) 2026 Astropulse, LLC; see
// THIRD_PARTY_NOTICES.txt): it finds the pseudo-pixel grid of "fake" pixel art (an AI generation, an upscale, a JPEG-crushed sprite) and collapses the picture to
// one true pixel per cell. Ported from the project's Rust core (src/*.rs): the same detectors, decision rules and two-stage reconstruction.
#include <cstdint>
#include <string>
#include <vector>

struct PixelFixDetection {
    double stepX = 8, stepY = 8; // cell size in picture pixels (sub-pixel)
    int cols = 0, rows = 0;      // the native resolution
    std::string consensus;       // the decision path: "fast:ac+rl(S)", "rl+ss+fu", "arbitrated", "fastmode:lowconf"...
    std::string confidence;      // "high" / "medium" / "low"
};

// Finds the grid. full = consensus + arbitration (best accuracy, up to ~10 s on a 4 MP picture); otherwise the fast mode. False when the picture is too small
// (under 16 pixels a side) or too large (over 4 megapixels: shrink it first).
bool PixelFixDetect(const uint8_t* rgba, int width, int height, bool full, PixelFixDetection& out);

// Two-stage packing on an even cols x rows grid: each cell's region is voted among k-means labels (crisp placement), its colour is the centre-weighted mean of the
// original pixels carrying that label (true colours). kColors 0 = adaptive. Returns cols * rows * 4 bytes of RGBA (alpha: the majority of the cell).
std::vector<uint8_t> PixelFixPack(const uint8_t* rgba, int width, int height, int cols, int rows, int kColors = 0);
