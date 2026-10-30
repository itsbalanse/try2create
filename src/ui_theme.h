#pragma once
// Look & feel: system fonts (Segoe UI + CJK fallbacks), the dark theme, DPI
// scaling and a few small widgets shared by every window.
#include "imgui.h"
#include <string>

namespace ui {

// Palette (also used for custom-drawn widgets).
inline ImVec4 kAccent{0.30f, 0.60f, 1.00f, 1.00f};
inline ImVec4 kAccentHover{0.42f, 0.69f, 1.00f, 1.00f};
inline ImVec4 kAccentActive{0.22f, 0.50f, 0.90f, 1.00f};
inline ImVec4 kGood{0.45f, 0.85f, 0.55f, 1.00f};
inline ImVec4 kWarn{1.00f, 0.78f, 0.33f, 1.00f};
inline ImVec4 kBad{1.00f, 0.45f, 0.42f, 1.00f};
inline ImVec4 kTextDim{0.55f, 0.59f, 0.66f, 1.00f};

// Builds the font atlas for `scale` (1.0 = 96 DPI) and re-applies the theme.
// Safe to call again after a DPI change (the caller must invalidate the
// renderer's device objects so the new atlas texture gets uploaded).
void Init(float scale, bool light = false);
bool IsLight();

// The accent colour (2026-10-30, after Open Source Canvas's eight - MIT, raph651/open-source-canvas): set it before Init.
int AccentCount();
const char* AccentName(int i);
ImVec4 AccentSwatch(int i);
void SetAccentIndex(int i);
int AccentIndex();

// Theme-dependent colours for custom-drawn widgets.
ImVec4 ViewerBackground();
ImU32 ThumbPlaceholder();
ImVec4 ChipColor();
ImVec4 ChipHoverColor();
ImVec4 ChipActiveColor();
ImU32 StarColor(bool filled);

float Scale();                        // current DPI scale
inline float Dp(float v) { return v * Scale(); } // device-independent px -> px
ImFont* BoldFont();                   // falls back to the regular font
ImFont* TitleFont();                  // large semibold face for headings
ImFont* DisplayFont();                // very large semibold face (a song's title): drawn smaller it stays smooth
ImFont* MonoFont();                   // small monospace face for labels and badges (Cascadia Mono / Consolas)

// A small, spaced-out monospace label in capitals ("REFERENCES"): the section label of Open Source Canvas's design
void Eyebrow(const char* text, const ImVec4* color = nullptr);

// Filled accent-coloured button for the main action of a dialog.
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// Puts the next (small) button on the same line only when it still fits, so a row of buttons wraps instead of running off the panel.
void SameLineIfFits(const char* nextLabel);

// "(?)" marker after the previous item that shows `text` on hover.
void HelpMarker(const char* text);

// Small rounded label drawn inline (advances the layout like a widget).
void Pill(const char* text, ImU32 background, ImU32 foreground);

// A section's label: an Eyebrow on its own line, with room under it.
void SectionHeading(const char* text);

// Folding a side pane away (2026-10-30): FoldButton is a small round button in the current window's top-right corner (call it first
// thing in the pane, so it wins the mouse over the pane's own widgets); true when clicked. A folded pane is a FoldStrip: a thin strip
// with its name down it, true when clicked (unfold).
bool FoldButton(const char* id, const char* tip);
bool FoldStrip(const char* id, const char* name, float width, float height);
void DrawPendingFoldButtons(); // once a frame, just before ImGui::Render (draws the fold buttons over their panes)

// The page's backdrop (night theme): Open Source Canvas's soft glows - blue from the top left, a little warm light at the top right - drawn
// into `dl` over the window's own background, before anything else.
void DrawBackdrop(ImDrawList* dl, ImVec2 min, ImVec2 max);

// Colour `c` as a packed ImU32 with the given alpha.
ImU32 WithAlpha(const ImVec4& c, float alpha);

// Human-readable helpers.
std::string FormatBytes(unsigned long long bytes);

} // namespace ui
