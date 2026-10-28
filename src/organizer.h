#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "nsfw_classifier.h"

// Reorganizes a whole folder tree by NSFW score into
//   <destRoot>\<year>\<nsfw|questionable|sfw|unsorted>\<file>
// (videos in video-nsfw, video-sfw, ...), where <year> is the file's
// last-modified year. Optionally the original subfolders are kept below the
// class folder. Images and (optionally)
// videos are scored; caption sidecars (img.txt / img.png.txt / img.json ...)
// travel with their image; other files go to <year>\other.
// Non-media files sitting directly in the source root are never touched.
// Every move is appended to a log so the run can be undone.
struct OrganizeOptions {
    std::wstring sourceRoot;
    std::wstring destRoot;              // empty = sourceRoot (organize in place)
    std::vector<std::wstring> excludes; // folders relative to sourceRoot, skipped entirely
    NsfwSource source = NsfwSource::Combined;
    float threshold = 0.5f;
    bool useQuestionable = true;
    float questionableLow = 0.3f;
    bool includeVideos = true;
    bool moveOtherFiles = true;
    bool copyInsteadOfMove = false;
    bool removeEmptyFolders = true;
    bool keepSubfolders = false; // true: <year>\<class>\<original subfolders>\<file>
    // Exact copies (byte-identical) of a file already in the collection, or
    // of another file in this run, go to <destRoot>\_duplicates instead.
    bool detectDuplicates = true;
    bool dryRun = false; // score + plan + report, but don't touch any files
};

enum OrganizePhase {
    kOrgIdle,
    kOrgScanning,
    kOrgLoadingModel,
    kOrgScoring,
    kOrgDuplicates,
    kOrgMoving,
    kOrgCleaning,
    kOrgUndoing,
};

struct OrganizeProgress {
    std::atomic<int> phase{kOrgIdle};
    std::atomic<int> current{0};
    std::atomic<int> total{0};
    std::atomic<bool> cancel{false};
};

// Access to the app's persistent model-score cache, so already-classified
// files aren't re-run and moved files keep their scores.
struct OrganizeHooks {
    std::function<float(const std::wstring& path, std::filesystem::file_time_type mtime, uintmax_t size)> cachedScore;
    std::function<void(const std::wstring& path, std::filesystem::file_time_type mtime, uintmax_t size, float score)>
        storeScore;
    std::function<void(const std::wstring& from, const std::wstring& to)> renameScore;
};

struct OrganizeResult {
    std::string message;
    bool filesChanged = false;
};

OrganizeResult RunOrganize(const OrganizeOptions& opts, NsfwClassifier& classifier, const OrganizeHooks& hooks,
                           OrganizeProgress& progress);

// Most recent organize log that hasn't been undone yet, or empty.
std::wstring LatestOrganizeLog();

// Moves everything recorded in `logPath` back where it came from (newest
// first), removes the folders that leaves empty, and marks the log undone.
OrganizeResult UndoOrganize(const std::wstring& logPath, const OrganizeHooks& hooks, OrganizeProgress& progress);
