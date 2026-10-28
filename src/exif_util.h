#pragma once
#include <string>
#include <cstdint>

// Minimal TIFF/Exif IFD walker: extracts UserComment (0x9286) and
// ImageDescription (0x010E) from a raw TIFF byte blob (the payload of a
// JPEG "Exif\0\0" APP1 segment, or a WEBP "EXIF" RIFF chunk).
struct ExifResult {
    std::string userComment;
    std::string imageDescription;
    std::string make;  // ComfyUI stores "workflow:{...}" here (and "prompt:{...}" in model) in WebP/JPEG
    std::string model;
    bool found = false;
};

ExifResult ParseExifTiff(const unsigned char* tiff, size_t len);
