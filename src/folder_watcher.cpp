#include "folder_watcher.h"

#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

void FolderWatcher::Start(const std::wstring& folder, bool recursive) {
    Stop();
    if (folder.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
    }
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return;
    running_ = true;
    thread_ = std::thread([this, folder, recursive]() { Run(folder, recursive); });
}

void FolderWatcher::Stop() {
    if (thread_.joinable()) {
        SetEvent(stopEvent_);
        thread_.join();
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    running_ = false;
}

std::vector<FolderWatcher::Change> FolderWatcher::Drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Change> out;
    out.swap(pending_);
    return out;
}

void FolderWatcher::Run(std::wstring folder, bool recursive) {
    HANDLE dir = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        running_ = false;
        return;
    }
    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    alignas(DWORD) std::vector<unsigned char> buffer(256 * 1024);
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                         FILE_NOTIFY_CHANGE_LAST_WRITE;

    while (true) {
        OVERLAPPED ov = {};
        ov.hEvent = ioEvent;
        ResetEvent(ioEvent);
        DWORD bytes = 0;
        if (!ReadDirectoryChangesW(dir, buffer.data(), static_cast<DWORD>(buffer.size()), recursive ? TRUE : FALSE, filter, &bytes, &ov, nullptr)) {
            break;
        }
        HANDLE waits[2] = {ioEvent, stopEvent_};
        DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (which != WAIT_OBJECT_0) { // stop requested
            CancelIo(dir);
            GetOverlappedResult(dir, &ov, &bytes, TRUE);
            break;
        }
        if (!GetOverlappedResult(dir, &ov, &bytes, FALSE)) break;

        std::vector<Change> batch;
        if (bytes == 0) {
            // The buffer overflowed: individual events were lost.
            batch.push_back({Kind::Overflow, L""});
        } else {
            const unsigned char* p = buffer.data();
            while (true) {
                const FILE_NOTIFY_INFORMATION* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
                std::wstring full = (fs::path(folder) / name).wstring();
                switch (info->Action) {
                    case FILE_ACTION_ADDED:
                    case FILE_ACTION_RENAMED_NEW_NAME: batch.push_back({Kind::Added, full}); break;
                    case FILE_ACTION_REMOVED:
                    case FILE_ACTION_RENAMED_OLD_NAME: batch.push_back({Kind::Removed, full}); break;
                    default: batch.push_back({Kind::Modified, full}); break;
                }
                if (info->NextEntryOffset == 0) break;
                p += info->NextEntryOffset;
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.insert(pending_.end(), batch.begin(), batch.end());
    }
    CloseHandle(ioEvent);
    CloseHandle(dir);
    running_ = false;
}
