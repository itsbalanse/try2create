#pragma once
#include <string>
#include <vector>
#include "metadata.h"

// Reads all tEXt/zTXt/iTXt chunks from a PNG file, decompressing as needed.
// Returns false only on hard I/O errors / not-a-PNG; a PNG with zero text
// chunks still returns true with an empty `out`.
bool ReadPngTextChunks(const std::wstring& path, std::vector<RawField>& out, std::string* error);
