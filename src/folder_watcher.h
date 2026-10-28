#pragma once
// Watches a folder tree for changes with ReadDirectoryChangesW on a background
// thread and hands the events to the UI thread.
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

class FolderWatcher {
public:
    enum class Kind { Added, Removed, Modified, Overflow };
    struct Change {
        Kind kind = Kind::Modified;
        std::wstring path; // absolute (empty for Overflow)
    };

    FolderWatcher() = default;
    ~FolderWatcher() { Stop(); }
    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

    // (Re)starts watching `folder`. Safe to call repeatedly; any previous watch is stopped first.
    void Start(const std::wstring& folder, bool recursive);
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // Takes the events collected since the last call.
    std::vector<Change> Drain();

private:
    void Run(std::wstring folder, bool recursive);

    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::vector<Change> pending_;
};
