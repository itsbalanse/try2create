#pragma once
// Real pixel art from "fake" pixel art. Diffusion models (and upscaled sprites) draw every art pixel as a soft cell of many picture pixels; this turns such a picture
// into one picture pixel per art pixel with a small palette: the dominant colour of each cell's middle, a palette made with k-means in OKLab (or a classic one),
// lone stray pixels cleaned up, the background cut out. The Retro Diffusion models draw on a fixed 8 x 8 grid; other pictures get their grid measured.
#include <cstdint>
#include <string>
#include <vector>

struct PixelLab {
    float L = 0, a = 0, b = 0;
};

struct PixelPalettePreset {
    const char* name;
    std::vector<uint32_t> colors; // 0xRRGGBB; empty = made from the picture
};
const std::vector<PixelPalettePreset>& PixelPalettePresets(); // [0] = "Made from the picture"

struct PixelOptions {
    int palette = 0;          // index into PixelPalettePresets()
    int colors = 16;          // a palette made from the picture: how many colours
    bool transparent = false; // the background (the colour that owns the border) becomes transparent
    bool clean = true;        // lone stray pixels take the colour of their neighbours
    bool outline = false;     // a dark one-pixel outline around what is left (with transparent)
};

// The colour of every art pixel (OKLab) and whether it is see-through.
struct PixelCells {
    int w = 0, h = 0;
    std::vector<PixelLab> lab;   // w * h
    std::vector<uint8_t> opaque; // w * h: 1 = the cell is mostly opaque in the source
};

// cols x rows equal cells over the whole picture (cell edges may fall between picture pixels).
PixelCells SampleCells(const uint8_t* rgba, int width, int height, int cols, int rows);
// Cells on a measured grid: xs has cols + 1 edges, ys rows + 1 (picture pixels).
PixelCells SampleCellsOnGrid(const uint8_t* rgba, int width, int height, const std::vector<float>& xs, const std::vector<float>& ys);
// Finds the pixel grid of fake pixel art (AI-drawn or upscaled): false when the picture has none (a photo). cellSize = the average cell width.
bool DetectPixelGrid(const uint8_t* rgba, int width, int height, std::vector<float>& xs, std::vector<float>& ys, float* cellSize = nullptr);

// One palette for one picture, or for a whole set of them (so they match).
std::vector<PixelLab> MakePixelPalette(const std::vector<const PixelCells*>& cells, const PixelOptions& options);
// The finished pixel art: w * h * 4 bytes of RGBA.
void RenderPixelArt(const PixelCells& cells, const std::vector<PixelLab>& palette, const PixelOptions& options, std::vector<uint8_t>& rgba);

PixelLab PixelRgbToLab(uint8_t r, uint8_t g, uint8_t b);
void PixelLabToRgb(const PixelLab& c, uint8_t out[3]);

// The part of a drawing that is not its background (the colour of its border), in the shape `aspect` (width / height) with a small margin, padded with the
// background where needed: an item or a character fills the pixel art instead of floating small in the middle.
std::vector<uint8_t> PixelSubjectCrop(const uint8_t* rgba, int width, int height, double aspect, int& outW, int& outH);

// A sprite sheet: frames side by side on a plain background (as Qwen draws them). The frames are found by the empty columns between them (else even slices)
// and put on frames of one size: the body's middle in the middle, the shared ground line at the bottom, so nothing jumps around. Sized so the figure of frame
// `refFrame` (its plain standing pose; -1 = the tallest frame) is `artTall` art pixels tall. out.rgba: frames * cellW x cellH picture pixels, to be packed onto frames * artW x artH art pixels.
struct PixelSheet {
    std::vector<uint8_t> rgba;
    int frames = 0, cellW = 0, cellH = 0, artW = 0, artH = 0;
};
bool PixelSliceSheet(const uint8_t* rgba, int width, int height, int expected, int refFrame, double artTall, PixelSheet& out);

// Picture helpers (RGBA, 4 bytes a pixel)
std::vector<uint8_t> PixelBoxDownscale(const uint8_t* rgba, int width, int height, int outW, int outH); // the average of each cell
std::vector<uint8_t> PixelNearestUpscale(const uint8_t* rgba, int width, int height, int factor);      // every pixel becomes factor x factor
std::vector<uint8_t> PixelMedian3(const uint8_t* rgba, int width, int height);                        // each channel's median of 3 x 3 (keeps edges, drops grain)
std::vector<uint8_t> PixelRoll(const uint8_t* rgba, int width, int height, int dx, int dy);           // shifted with wrap-around
bool PixelLoadRGBA(const std::wstring& path, std::vector<uint8_t>& rgba, int& width, int& height, std::string* error = nullptr);
bool PixelSavePng(const std::wstring& path, const uint8_t* rgba, int width, int height);
