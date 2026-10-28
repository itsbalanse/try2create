#pragma once
#include <string>
#include <vector>
#include "metadata.h"

// Reads EXIF (UserComment/ImageDescription) and raw XMP text from a JPEG file.
bool ReadJpegTextFields(const std::wstring& path, std::vector<RawField>& out, std::string* error);

// Reads EXIF and raw XMP chunks from a WEBP (RIFF) file.
bool ReadWebpTextFields(const std::wstring& path, std::vector<RawField>& out, std::string* error);
