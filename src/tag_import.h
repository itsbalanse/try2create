#pragma once
#include <string>
#include <vector>

struct TagImportResult {
    bool success = false;
    std::string message;
    std::vector<std::string> tags; // flattened, deduplicated, in first-seen order
};

// Reads tags out of a CSV or JSON file previously produced by this app's
// "Scrape Folder..." / "Export Selected..." features (dispatches on the
// file extension; unknown extensions are treated as CSV).
TagImportResult ImportTagsFromFile(const std::wstring& path);
