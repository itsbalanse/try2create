#pragma once
// Builds new file names from an image's metadata using a template such as
// "{date}_{model}_{seed}" or "{n:4}_{prompt:30}".
#include <string>
#include <utility>
#include <vector>

#include "app_state.h"

struct RenamePlanItem {
    int index = 0;           // gallery index
    std::wstring from, to;   // full paths; equal when nothing changes
    bool changed = false;
    bool adjusted = false;   // a number was appended to avoid a duplicate or an existing file
};

// Makes `s` safe as a file name part: replaces characters Windows forbids,
// collapses whitespace, trims dots/spaces, avoids reserved device names and
// limits the length (in bytes of UTF-8, cut on a character boundary).
std::string SanitizeFileName(const std::string& s, size_t maxBytes = 120);

// Expands one template for one image (no extension). `sequence` feeds {n}.
// Unknown tokens leave `error` set (and are dropped from the result).
std::string ExpandRenameTemplate(const std::string& tmpl, const ImageEntry& e, int sequence, std::string* error = nullptr);

// Plans the renames for `indices` (in that order): same folder, original
// extension, duplicates and existing files get " (2)", " (3)" ... appended.
std::vector<RenamePlanItem> BuildRenamePlan(const AppState& app, const std::vector<int>& indices, const std::string& tmpl,
                                            int startNumber, std::string* error = nullptr);

// Token name + description pairs for the help list.
const std::vector<std::pair<const char*, const char*>>& RenameTokenHelp();
