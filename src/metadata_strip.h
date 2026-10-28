#pragma once
// Writes copies of images with their embedded metadata (prompts, seeds,
// EXIF, XMP, ...) removed. Works on the file container only, so pixels are
// never re-encoded: the copy is bit-for-bit the same picture.
#include <string>

enum class StripMode {
    RemoveAll,       // no metadata at all
    KeepPromptOnly,  // PNG: keep just the positive/negative prompt as a "parameters" text chunk
                     // (other formats can't carry it and are stripped completely)
};

// Reads `src`, writes the cleaned bytes to `dst` (created/overwritten; parent
// folders are created). `promptText` is only used for KeepPromptOnly on PNGs
// and is the already-formatted text to store (may be empty).
// Unknown formats are copied unchanged. Returns false with `error` on failure.
bool WriteCleanCopy(const std::wstring& src, const std::wstring& dst, StripMode mode, const std::string& promptText,
                    std::string* error);

// The byte-level cleaners, exposed for testing. Return false if the data isn't
// valid for the format (the caller then falls back to an unchanged copy).
bool StripPng(const std::string& in, std::string& out, StripMode mode, const std::string& promptText);
bool StripJpeg(const std::string& in, std::string& out);
bool StripWebp(const std::string& in, std::string& out);

// True for formats this module can clean (PNG, JPEG, WebP).
bool CanStripMetadata(const std::wstring& path);
