#pragma once
#include <string>
#include <vector>
#include <atomic>

enum class ScrapeFormat { Csv, Json };

struct ScrapeResult {
    bool success = false;
    bool cancelled = false;
    int filesScanned = 0;
    int filesWithMetadata = 0;
    std::string message;
};

// Progress/cancellation handle shared between a caller (usually the UI
// thread, reading current/total) and a scrape running on a background
// thread (writing current/total, checking cancelRequested periodically).
struct ScrapeProgress {
    std::atomic<int> current{0};
    std::atomic<int> total{0};
    std::atomic<bool> cancelRequested{false};
};

// Recursively (or not) scans `folder` for supported images, extracts
// metadata + tags from each, and writes a single combined CSV or JSON file
// to `outputFile`. `progress` may be null.
ScrapeResult ScrapeFolder(const std::wstring& folder, bool recursive, ScrapeFormat format,
                          const std::wstring& outputFile, ScrapeProgress* progress = nullptr);

// Same as ScrapeFolder, but over an explicit file list (e.g. the gallery's
// current multi-selection) rather than scanning a directory.
ScrapeResult ScrapeFiles(const std::vector<std::wstring>& files, ScrapeFormat format, const std::wstring& outputFile,
                         ScrapeProgress* progress = nullptr);
