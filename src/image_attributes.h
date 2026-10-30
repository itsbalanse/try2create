#pragma once
// Derived, searchable facts about an image: when it was made, how many people
// it shows (estimated from the prompt) and the size it was generated at.
#include <filesystem>
#include <string>
#include "metadata.h"

// Local-time year and month (1-12) of a file timestamp; 0/0 if unavailable.
void FileTimeYearMonth(std::filesystem::file_time_type t, int& year, int& month);

// Estimated number of people from a positive prompt. Understands Danbooru tags
// ("1girl, 2boys, solo, no humans") and plain English ("two women", "a man").
// Returns -1 when the prompt doesn't say, 0 for "no humans", otherwise 1..6
// (6 means six or more / a crowd).
int EstimatePeopleCount(const std::string& positivePrompt);

// Helper pictures: what ComfyUI saves along the way, not results. 0 no, 1 depth map, 2 mask, 3 pose. HelperPixels looks at a thumbnail (RGBA);
// HelperKind takes that with the file's path below the opened folder and its prompt (black and white art someone asked for stays a picture).
int HelperPixels(const unsigned char* rgba, int w, int h);
int HelperKind(const std::wstring& relativePath, int pixels, const std::string& positivePrompt);

// "unknown", "none", "1 (solo)", "2", ..., "6+".
std::string PeopleLabel(int count);

// Parses "512x768" (the "Size" parameter). Returns false if it isn't that form.
bool ParseSizeParam(const std::string& value, int& width, int& height);

// Month number for "3", "03", "mar" or "march"; 0 if it isn't a month.
int ParseMonth(const std::string& lowerText);

const char* MonthName(int month); // "January".."December", "" if out of range

// Display name of the tool that made an image ("ComfyUI", "AUTOMATIC1111 / compatible fork", ...).
const char* GeneratorName(Generator g);

// The prompt and settings as AUTOMATIC1111 "parameters" text, ready to paste into
// most generators ("prompt\nNegative prompt: ...\nSteps: 20, Sampler: ...").
std::string BuildA1111Text(const ImageMetadata& meta);

// First folder component of a relative path ("a\\b\\x.png" -> "a"), or "" if the
// file sits directly in the root.
std::string TopFolderOf(const std::wstring& relativePath);

// "2026-03-05 14:25" in local time ("" if unavailable).
std::string FormatLocalDateTime(std::filesystem::file_time_type t);
