#pragma once
#include <string>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include "metadata.h"

// Persists decoded thumbnails + parsed metadata to disk (under
// %LOCALAPPDATA%\Try2Create\cache) so reopening a folder is fast even
// after restarting the app. Keyed by a caller-supplied lowercased absolute
// path; entries are invalidated by last-write-time/size mismatch.
class DiskCache {
public:
    struct Record {
        std::filesystem::file_time_type mtime{};
        uintmax_t size = 0;
        int thumbW = 0;
        int thumbH = 0;
        ImageMetadata metadata;
        std::string searchTextLower;
        std::wstring thumbFileName; // relative to the thumbs/ subdirectory
        uint64_t lastAccessTick = 0;
    };

    // Reads the on-disk index (if any). Safe to call once at startup;
    // creates the cache directory if it doesn't exist yet.
    void Load();

    // Writes the current index to disk. Cheap relative to re-decoding, but
    // still O(total cached entries), so callers should batch calls (e.g.
    // once per folder load) rather than call this per file. If the cache
    // has grown past kMaxEntries, evicts the least-recently-used entries
    // (and their thumbnail files) first so it doesn't grow forever.
    void Save();

    // Updates the entry's last-access tick on a hit (so it survives eviction
    // longer than files that haven't been revisited).
    bool TryGet(const std::wstring& lowerKey, Record& out);

    // Encodes `rgba` as PNG under the cache's thumbs/ directory and records
    // the entry in memory (call Save() afterward to persist the index).
    void Put(const std::wstring& lowerKey, std::filesystem::file_time_type mtime, uintmax_t size,
             const ImageMetadata& metadata, const std::string& searchTextLower, const unsigned char* rgba, int w,
             int h);

    // Reads the persisted thumbnail bytes for a record (still PNG-encoded;
    // caller decodes/uploads it).
    bool ReadThumbBytes(const Record& record, std::vector<unsigned char>& outBytes) const;

    size_t EntryCount() const;

    // Moves a record to a new key (the file was renamed); the thumbnail file stays as is.
    void Rekey(const std::wstring& fromKey, const std::wstring& toKey);

private:
    static constexpr size_t kMaxEntries = 30000; // eviction target; keeps the cache from growing forever

    mutable std::mutex mutex_;
    std::unordered_map<std::wstring, Record> records_;
    uint64_t accessCounter_ = 0;
    std::wstring cacheDir_;
    std::wstring thumbsDir_;
    bool ready_ = false;
};
