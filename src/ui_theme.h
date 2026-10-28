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

// Filled accent-coloured button for the main action of a dialog.
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// Puts the next (small) button on the same line only when it still fits, so a row of buttons wraps instead of running off the panel.
void SameLineIfFits(const char* nextLabel);

// "(?)" marker after the previous item that shows `text` on hover.
void HelpMarker(const char* text);

// Small rounded label drawn inline (advances the layout like a widget).
void Pill(const char* text, ImU32 background, ImU32 foreground);

// Bold dim heading with a hairline underneath.
void SectionHeading(const char* text);

// Colour `c` as a packed ImU32 with the given alpha.
ImU32 WithAlpha(const ImVec4& c, float alpha);

// Human-readable helpers.
std::string FormatBytes(unsigned long long bytes);

} // namespace ui
