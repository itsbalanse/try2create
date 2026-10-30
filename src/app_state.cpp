#include "app_state.h"
#include "image_attributes.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <windows.h>
#include <shellapi.h>
#include <set>
#include "json.hpp"
#include "stb_image.h"
#include <chrono>
#include <cstring>
#include "video_util.h"

namespace fs = std::filesystem;

namespace {

bool IsSupportedImage(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp" ||
           ext == L".bmp" || ext == L".gif" || ext == L".mp4" || ext == L".webm" || ext == L".mkv" ||
           ext == L".mov" || ext == L".m4v" || ext == L".avi";
}

std::wstring ToLowerCopy(const std::wstring& w) {
    std::wstring r = w;
    std::transform(r.begin(), r.end(), r.begin(), ::towlower);
    return r;
}

std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

// Lowercase, backslashes, trailing "\": a form in which "is inside" is a prefix test.
std::wstring NormalizedDir(const std::wstring& path) {
    std::wstring s = ToLowerCopy(path);
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (!s.empty() && s.back() == L'\\') s.pop_back();
    if (!s.empty()) s.push_back(L'\\');
    return s;
}

std::wstring CopyKey(const std::wstring& path, uintmax_t size, std::filesystem::file_time_type mtime) {
    return ToLowerCopy(fs::path(path).filename().wstring()) + L"|" + std::to_wstring(size) + L"|" +
           std::to_wstring(static_cast<long long>(mtime.time_since_epoch().count()));
}

std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::string BuildSearchTextLower(const ImageMetadata& md, const std::wstring& path) {
    std::string combined = md.parsed.positivePrompt + " " + md.parsed.negativePrompt + " " +
                            NarrowUtf8(fs::path(path).filename().wstring());
    std::transform(combined.begin(), combined.end(), combined.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return combined;
}

struct PendingFile {
    std::wstring path;
    fs::file_time_type mtime{};
    uintmax_t size = 0;
};

constexpr int kThumbnailMaxDim = 256;

} // namespace

AppState::~AppState() {
    nsfwAskStop_ = true;
    if (nsfwAskFut_.valid()) nsfwAskFut_.wait();
    videoPlayer.reset();
    CancelQualityJob();
    wf.batch.Stop();
    CancelListing();
    StopLibraryThumbs();
    ReleaseBlurredCopies();
    if (libThread_.joinable()) libThread_.join();
    ReleaseImage(visionLibImage);
    if (describedThread_.joinable()) describedThread_.join();
    CancelVisionFolder();
    CancelCleanExport();
    CancelRename();
    watcher_.Stop();
    StopLiveWorker();
    if (diskCacheReady_.valid()) diskCacheReady_.wait();
    CancelSimilarScan();
    CancelPeopleScan();
    SaveRatingsIfDirty(true);
    CloseCompare();
    CancelNsfwJob();
    CancelLoading();
    CancelScrape();
    ReleaseImage(currentTexture);
    // thumbSrv on entries is non-owning (the cache owns it); release the cache itself.
    for (auto& [key, cached] : thumbCache_) {
        if (cached.srv) cached.srv->Release();
    }
    for (auto* srv : retiredSrvs_) srv->Release();
}

void AppState::CancelLoading() {
    if (loaderThread_.joinable()) {
        cancelLoading_ = true;
        loaderThread_.join();
    }
    cancelLoading_ = false;
    loaderActive_ = false;

    // Any pending results reference cache-owned textures now (or nothing, if
    // decode failed), so just drop the queue without releasing anything.
    std::lock_guard<std::mutex> lock(resultsMutex_);
    pendingResults_.clear();
}

std::vector<ListedImage> ListImageFiles(const std::wstring& folder, bool recursive, const std::wstring& excludeDir,
                                        const std::atomic<bool>* cancel, std::atomic<int>* progress) {
    std::vector<ListedImage> out;
    const std::wstring excludeNorm = NormalizedDir(excludeDir);
    const std::wstring folderNorm = NormalizedDir(folder);
    // Opening the excluded folder itself (or something inside it) is deliberate: show it.
    const bool useExclusion = !excludeNorm.empty() && folderNorm.compare(0, excludeNorm.size(), excludeNorm) != 0;

    std::wstring root = folder;
    std::replace(root.begin(), root.end(), L'/', L'\\');
    while (root.size() > 3 && root.back() == L'\\') root.pop_back();
    const size_t rootLen = root.size() + (root.back() == L'\\' ? 0 : 1); // characters before the relative part

    // Native enumeration: the directory listing already carries size and time, so
    // there is no per-file stat (the std::filesystem walk plus fs::relative cost
    // ~13 s on 16,000 images; this is a fraction of that).
    std::vector<std::wstring> pending{root};
    WIN32_FIND_DATAW fd;
    while (!pending.empty()) {
        if (cancel && cancel->load()) return {};
        std::wstring dir = std::move(pending.back());
        pending.pop_back();
        std::wstring pattern = dir + (dir.back() == L'\\' ? L"*" : L"\\*");
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const wchar_t* name = fd.cFileName;
            if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!recursive || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue; // no junction/symlink loops
                std::wstring sub = dir + (dir.back() == L'\\' ? L"" : L"\\") + name;
                if (useExclusion && NormalizedDir(sub).compare(0, excludeNorm.size(), excludeNorm) == 0) continue;
                pending.push_back(std::move(sub));
                continue;
            }
            if (name[0] == L'.' && name[1] == L'_') continue; // macOS "._name.png" resource-fork stubs are never pictures
            // supported image extension?
            const wchar_t* dot = wcsrchr(name, L'.');
            if (!dot) continue;
            const wchar_t* ext = dot + 1;
            if (_wcsicmp(ext, L"png") && _wcsicmp(ext, L"jpg") && _wcsicmp(ext, L"jpeg") && _wcsicmp(ext, L"webp") &&
                _wcsicmp(ext, L"bmp") && _wcsicmp(ext, L"gif") && _wcsicmp(ext, L"mp4") && _wcsicmp(ext, L"webm") &&
                _wcsicmp(ext, L"mkv") && _wcsicmp(ext, L"mov") && _wcsicmp(ext, L"m4v") && _wcsicmp(ext, L"avi")) {
                continue;
            }
            ListedImage f;
            f.path = dir + (dir.back() == L'\\' ? L"" : L"\\") + name;
            f.relative = f.path.substr(rootLen);
            f.size = (static_cast<uintmax_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            ULARGE_INTEGER t;
            t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            f.mtime = fs::file_time_type(fs::file_time_type::duration(static_cast<long long>(t.QuadPart)));
            out.push_back(std::move(f));
            if (progress && (out.size() & 255) == 0) progress->store(static_cast<int>(out.size()));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (progress) progress->store(static_cast<int>(out.size()));
    return out;
}

int DropDuplicateCopies(std::vector<ListedImage>& files, const std::wstring& excludeDir) {
    const std::wstring excludeNorm = NormalizedDir(excludeDir);
    std::unordered_map<std::wstring, std::vector<size_t>> groups;
    groups.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) groups[CopyKey(files[i].path, files[i].size, files[i].mtime)].push_back(i);

    std::vector<char> drop(files.size(), 0);
    int dropped = 0;
    for (auto& [key, members] : groups) {
        if (members.size() < 2) continue;
        auto score = [&](size_t i) {
            std::wstring norm = NormalizedDir(files[i].path);
            const bool inExcluded = !excludeNorm.empty() && norm.compare(0, excludeNorm.size(), excludeNorm) == 0;
            const auto depth = std::count(files[i].path.begin(), files[i].path.end(), L'\\');
            return std::make_tuple(inExcluded ? 1 : 0, depth, ToLowerCopy(files[i].path));
        };
        size_t keep = members[0];
        for (size_t m : members) {
            if (score(m) < score(keep)) keep = m;
        }
        for (size_t m : members) {
            if (m != keep) {
                drop[m] = 1;
                ++dropped;
            }
        }
    }
    if (dropped > 0) {
        std::vector<ListedImage> kept;
        kept.reserve(files.size() - dropped);
        for (size_t i = 0; i < files.size(); ++i) {
            if (!drop[i]) kept.push_back(std::move(files[i]));
        }
        files = std::move(kept);
    }
    return dropped;
}

void AppState::CancelListing() {
    if (listThread_.joinable()) {
        listCancel_ = true;
        listThread_.join();
    }
    listCancel_ = false;
    listDone_ = false;
    listing_ = false;
}

void AppState::OpenFolder(const std::wstring& folder, const std::wstring& selectAfter) {
    StopVideo();
    CancelListing();
    CancelNsfwJob(); // its entry indices are about to become meaningless
    CancelLoading();

    ReleaseImage(currentTexture);
    currentMetadata = ImageMetadata{};
    CancelSimilarScan();
    CancelPeopleScan();
    CloseCompare();
    ClearRestrict();
    similarGroups.clear();
    similarMessage.clear();
    filters.folder.clear();
    filters.dir.clear();
    entries.clear();
    selectedIndex = -1;
    multiSelected.clear();
    lastClickedIndex = -1;
    currentFolder = folder;
    filterEpoch++;
    AddRecentFolder(folder);
    loadedCount_ = 0;
    totalCount_ = 0;
    missingCount_ = 0;
    hiddenCopies_ = 0;
    watchPending_.clear();
    liveFollowIndex_ = -1;
    pathIndex_.clear();
    copyIndex_.clear();
    watcher_.Stop();
    watchedFolder_.clear();

    // The scan (directory walk, duplicate detection, sorting, index building) runs in the
    // background; PumpFolderListing() takes over the result. The Vision output folder is left
    // out, and so are extra copies of the same image (it holds a copy of everything described).
    const std::wstring excludeDir = hideVisionFolder ? WidenUtf8(visionOutputDir) : std::wstring();
    const bool recursive = scanSubfolders, hideCopies = hideDuplicateCopies;
    listingFound_ = 0;
    listDone_ = false;
    listing_ = true;
    listThread_ = std::thread([this, folder, selectAfter, excludeDir, recursive, hideCopies]() {
        std::vector<ImageEntry> out;
        std::unordered_map<std::wstring, int> pathIndex, copyIndex;
        int hidden = 0;
        try {
            std::vector<ListedImage> listed = ListImageFiles(folder, recursive, excludeDir, &listCancel_, &listingFound_);
            if (listCancel_.load()) return;
            hidden = hideCopies ? DropDuplicateCopies(listed, excludeDir) : 0;
            std::sort(listed.begin(), listed.end(), [](const ListedImage& a, const ListedImage& b) {
                return _wcsicmp(a.relative.c_str(), b.relative.c_str()) < 0;
            });
            out.reserve(listed.size());
            pathIndex.reserve(listed.size());
            for (ListedImage& f : listed) {
                ImageEntry entry;
                entry.path = std::move(f.path);
                entry.displayName = std::move(f.relative);
                entry.size = f.size;
                entry.mtime = f.mtime;
                FileTimeYearMonth(f.mtime, entry.year, entry.month);
                const int index = static_cast<int>(out.size());
                pathIndex[ToLowerCopy(entry.path)] = index;
                if (hideCopies) copyIndex[CopyKey(entry.path, entry.size, entry.mtime)] = index;
                out.push_back(std::move(entry));
            }
        } catch (...) {
            out.clear();
        }
        {
            std::lock_guard<std::mutex> lock(listMutex_);
            listEntries_ = std::move(out);
            listPathIndex_ = std::move(pathIndex);
            listCopyIndex_ = std::move(copyIndex);
            listHidden_ = hidden;
            listSelectAfter_ = selectAfter;
        }
        listDone_ = true;
    });
}

// Takes the finished scan: installs the entries, starts watching and the thumbnail loader.
void AppState::PumpFolderListing() {
    if (!listDone_.load()) return;
    if (listThread_.joinable()) listThread_.join();
    std::wstring selectAfter;
    {
        std::lock_guard<std::mutex> lock(listMutex_);
        entries = std::move(listEntries_);
        listEntries_.clear();
        pathIndex_ = std::move(listPathIndex_);
        copyIndex_ = std::move(listCopyIndex_);
        hiddenCopies_ = listHidden_;
        selectAfter = std::move(listSelectAfter_);
    }
    listDone_ = false;
    listing_ = false;
    totalCount_ = static_cast<int>(entries.size());
    entriesSortedByName_ = true;
    filterEpoch++;
    lastEpochTick_ = GetTickCount64();

    if (watchFolder) {
        watcher_.Start(currentFolder, scanSubfolders);
        watchedFolder_ = currentFolder;
        watchedRecursive_ = scanSubfolders;
    }
    if (entries.empty()) return;

    if (!selectAfter.empty()) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (_wcsicmp(entries[i].path.c_str(), selectAfter.c_str()) == 0) {
                SelectIndex(static_cast<int>(i));
                break;
            }
        }
    }

    // Thumbnail/metadata decoding happens on a background thread so the UI
    // stays responsive no matter how large the folder is. Files already seen
    // (same path, mtime, and size) are served straight from the cache, so
    // reopening a folder you've already browsed this session is instant.
    struct PendingFile {
        std::wstring path;
        fs::file_time_type mtime{};
        uintmax_t size = 0;
    };
    std::vector<PendingFile> pendingFiles;
    pendingFiles.reserve(entries.size());
    for (const ImageEntry& e : entries) pendingFiles.push_back({e.path, e.mtime, e.size});
    cancelLoading_ = false;
    loaderActive_ = true;
    loaderThread_ = std::thread([this, pendingFiles = std::move(pendingFiles)]() {
        if (diskCacheReady_.valid()) diskCacheReady_.wait();
        // Several workers: with the cache warm the work is reading small thumbnail files and uploading them,
        // which parallelises well; a cold scan is dominated by decoding and parses in parallel too.
        const size_t total = pendingFiles.size();
        const unsigned hw = std::thread::hardware_concurrency();
        const unsigned workers = std::clamp<unsigned>(hw > 3 ? hw / 2 : 2, 2, 6);
        std::atomic<size_t> next{0};
        std::atomic<unsigned> running{workers};
        std::vector<std::thread> pool;
        for (unsigned w = 0; w < workers; ++w) {
            pool.emplace_back([&]() {
                for (;;) {
                    if (cancelLoading_.load()) break;
                    const size_t i = next.fetch_add(1);
                    if (i >= total) break;
                    const PendingFile& pf = pendingFiles[i];
                    try {
                        ProcessFile(i, pf.path, pf.mtime, pf.size, /*persist=*/true);
                    } catch (...) {
                    }
                    loadedCount_.fetch_add(1);
                }
                --running;
            });
        }
        // Save the cache index now and then while working, so closing the app (or switching folders)
        // halfway never throws the finished work away.
        auto lastSave = std::chrono::steady_clock::now();
        size_t savedAt = 0;
        while (running.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const auto now = std::chrono::steady_clock::now();
            const size_t done = static_cast<size_t>(loadedCount_.load());
            if (now - lastSave > std::chrono::seconds(25) && done > savedAt + 200) {
                try {
                    diskCache_.Save();
                } catch (...) {
                }
                lastSave = std::chrono::steady_clock::now();
                savedAt = done;
            }
        }
        for (std::thread& t : pool) t.join();

        try {
            diskCache_.Save();
        } catch (...) {
            // Losing this session's cache persistence is far better than
            // crashing right as loading finishes.
        }
        loaderActive_ = false;
    });
}

// Gallery refreshes (filter + sort + layout) are expensive on big folders, so while
// thumbnails stream in they happen a few times a second instead of every frame.
void AppState::BumpEpochThrottled() {
    const unsigned long long now = GetTickCount64();
    if (now - lastEpochTick_ >= 300 || !loaderActive_.load()) {
        filterEpoch++;
        lastEpochTick_ = now;
        epochPending_ = false;
    } else {
        epochPending_ = true;
    }
}

// Loads the thumbnail + metadata for one file (memory cache, then disk cache,
// then a full decode) and queues the result for the UI thread. Used by the
// folder loader and by live updates. `persist` = also write the disk cache.
void AppState::ProcessFile(size_t index, const std::wstring& path, fs::file_time_type mtime, uintmax_t size, bool persist) {
    const std::wstring key = ToLowerCopy(path);


    // A single malformed/unusual file must never take down the whole
    // background thread (and with it, the app): skip it and move on.
    try {
        LoadedThumbResult result;
        result.index = index;

        bool cacheHit = false;
        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            auto found = thumbCache_.find(key);
            if (found != thumbCache_.end() && found->second.mtime == mtime &&
                found->second.size == size) {
                result.srv = found->second.srv;
                result.w = found->second.w;
                result.h = found->second.h;
                result.helper = found->second.helper;
                result.metadata = found->second.metadata;
                result.searchTextLower = found->second.searchTextLower;
                cacheHit = true;
            }
        }

        if (!cacheHit) {
            // Next, try the on-disk cache: a hit here still means reading and
            // decoding a small persisted thumbnail PNG (fast) instead of the
            // full original image, and skips re-parsing metadata entirely.
            DiskCache::Record diskRec;
            bool diskHit =
                diskCache_.TryGet(key, diskRec) && diskRec.mtime == mtime && diskRec.size == size;
            if (diskHit && diskRec.metadata.parserRev < kParserRevision) {
                // Saved by an older reader: only the files whose reading has improved are read again.
                const std::wstring ext = fs::path(path).extension().wstring();
                const bool improved = (diskRec.metadata.hasMetadata && diskRec.metadata.generator != Generator::Automatic1111) ||
                                      diskRec.metadata.isVideo || IsVideoPath(path) || _wcsicmp(ext.c_str(), L".webp") == 0 ||
                                      _wcsicmp(ext.c_str(), L".jpg") == 0 || _wcsicmp(ext.c_str(), L".jpeg") == 0;
                if (improved) diskHit = false;
            }

            ID3D11ShaderResourceView* srv = nullptr;
            int w = 0, h = 0, helper = 0;
            ImageMetadata metadata;
            std::string searchText;

            if (diskHit) {
                std::vector<unsigned char> thumbBytes, thumbRGBA;
                LoadedImage thumb;
                std::string err;
                if (diskCache_.ReadThumbBytes(diskRec, thumbBytes) &&
                    LoadImageTextureFromMemory(device_, thumbBytes.data(), thumbBytes.size(), thumb, &err, &thumbRGBA)) {
                    srv = thumb.srv;
                    w = thumb.width;
                    h = thumb.height;
                    helper = HelperPixels(thumbRGBA.data(), w, h);
                    metadata = std::move(diskRec.metadata);
                    searchText = std::move(diskRec.searchTextLower);
                    SlimMetadata(metadata);
                } else {
                    diskHit = false; // missing/corrupt thumb file: fall back to a full decode
                }
            }

            if (!diskHit) {
                LoadedImage thumb;
                std::string err;
                std::vector<unsigned char> rawRGBA;
                if (LoadImageThumbnail(device_, path, kThumbnailMaxDim, thumb, &rawRGBA, &err)) {
                    srv = thumb.srv;
                    w = thumb.width;
                    h = thumb.height;
                    helper = HelperPixels(rawRGBA.data(), w, h);
                }
                metadata = ReadImageMetadata(path);
                searchText = BuildSearchTextLower(metadata, path);
                SlimMetadata(metadata); // the gallery and the cache don't need whole workflows

                if (persist && srv && !rawRGBA.empty()) {
                    diskCache_.Put(key, mtime, size, metadata, searchText, rawRGBA.data(), w, h);
                }
            }

            result.srv = srv;
            result.w = w;
            result.h = h;
            result.helper = helper;
            result.metadata = metadata;
            result.searchTextLower = searchText;

            CachedThumb newCached;
            newCached.srv = srv;
            newCached.w = w;
            newCached.h = h;
            newCached.helper = helper;
            newCached.metadata = std::move(metadata);
            newCached.searchTextLower = std::move(searchText);
            newCached.mtime = mtime;
            newCached.size = size;

            std::lock_guard<std::mutex> lock(cacheMutex_);
            auto existing = thumbCache_.find(key);
            if (existing != thumbCache_.end() && existing->second.srv && existing->second.srv != srv) {
                if (persist) ReleaseAfterFrame(existing->second.srv); // file changed since it was last cached
                else retiredSrvs_.push_back(existing->second.srv); // may still be drawn this frame
            }
            thumbCache_[key] = std::move(newCached);
        }

        {
            std::lock_guard<std::mutex> lock(resultsMutex_);
            pendingResults_.push_back(std::move(result));
        }
    } catch (...) {
        // Leave this file unprocessed rather than crashing the loader;
        // it'll just show as a placeholder in the gallery.
    }
}

void AppState::PumpBackgroundResults() {
    PumpFolderListing();
    if (epochPending_) BumpEpochThrottled();
    std::vector<LoadedThumbResult> drained;
    {
        std::lock_guard<std::mutex> lock(resultsMutex_);
        if (pendingResults_.empty()) return;
        drained.swap(pendingResults_);
    }

    BumpEpochThrottled();
    bool shouldAutoSelectFirst = (selectedIndex < 0);

    for (auto& r : drained) {
        if (r.index >= entries.size()) continue;
        ImageEntry& e = entries[r.index];
        e.thumbSrv = r.srv;
        e.thumbW = r.w;
        e.thumbH = r.h;
        e.metadata = std::move(r.metadata);
        e.searchTextLower = std::move(r.searchTextLower);
        e.thumbReady = true;
        e.helper = e.metadata.isVideo ? 0 : HelperKind(e.displayName, r.helper, e.metadata.parsed.positivePrompt);
        e.nsfwPromptScore =
            e.metadata.parsed.positivePrompt.empty() ? -1.0f : PromptNsfwScore(e.metadata.parsed.positivePrompt);
        e.nsfwImageScore = LookupNsfwCache(e.path, e.mtime, e.size);
        LookupQualityCache(e);
        e.people = EstimatePeopleCount(e.metadata.parsed.positivePrompt);
        e.peopleFromImage = false;
        {
            std::lock_guard<std::mutex> lock(peopleCacheMutex_);
            auto pit = peopleCache_.find(ToLowerCopy(e.path));
            if (pit != peopleCache_.end() && pit->second.mtime == static_cast<int64_t>(e.mtime.time_since_epoch().count()) && pit->second.size == e.size) {
                e.people = pit->second.count;
                e.peopleFromImage = true;
            }
        }
        {
            std::lock_guard<std::mutex> lock(ratingsMutex_);
            auto rit = ratings_.find(ToLowerCopy(e.path));
            e.rating = rit == ratings_.end() ? 0 : rit->second;
        }
        e.visionMask = VisionMaskFor(e);
        e.genW = e.genH = 0;
        e.modelLower.clear();
        for (const auto& kv : e.metadata.parsed.params) {
            if (_stricmp(kv.first.c_str(), "Size") == 0) ParseSizeParam(kv.second, e.genW, e.genH);
            else if (_stricmp(kv.first.c_str(), "Model") == 0) {
                e.modelLower = kv.second;
                std::transform(e.modelLower.begin(), e.modelLower.end(), e.modelLower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            }
        }
    }

    if (shouldAutoSelectFirst && !entries.empty() && entries[0].thumbReady) {
        // the first as the gallery shows it, and no scrolling: the first by name could be the last in a sort by date, which opened the gallery at its bottom
        SelectIndex(!visibleOrder.empty() && visibleOrder.front() < static_cast<int>(entries.size()) ? visibleOrder.front() : 0);
        scrollToSelected = false;
    }
    if (liveFollowIndex_ >= 0 && liveFollowIndex_ < static_cast<int>(entries.size()) && entries[liveFollowIndex_].thumbReady) {
        SelectIndex(liveFollowIndex_);
        scrollToSelected = true;
        liveFollowIndex_ = -1;
    }
}

void AppState::GalleryRemember() {
    if (currentFolder.empty()) return;
    GalleryPlace p;
    p.folder = currentFolder;
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(entries.size())) p.path = entries[selectedIndex].path;
    p.scrollY = gridScrollNow;
    if (!galleryBack.empty() && galleryBack.back().folder == p.folder && galleryBack.back().path == p.path && galleryBack.back().scrollY == p.scrollY) return;
    galleryBack.push_back(std::move(p));
    if (galleryBack.size() > 30) galleryBack.erase(galleryBack.begin());
}

void AppState::GalleryGoBack() {
    if (galleryBack.empty()) return;
    const GalleryPlace p = galleryBack.back();
    galleryBack.pop_back();
    if (_wcsicmp(p.folder.c_str(), currentFolder.c_str()) != 0) {
        OpenFolder(p.folder, p.path); // (the picture you had, brought into view when the folder is in)
        return;
    }
    auto it = pathIndex_.find(ToLowerCopy(p.path));
    if (it != pathIndex_.end() && it->second >= 0 && it->second < static_cast<int>(entries.size())) SelectIndex(it->second);
    scrollToSelected = false;
    gridScrollTo = p.scrollY;
}

void AppState::OpenFile(const std::wstring& file) {
    // already in the open folder (or a folder below it): found there, the gallery stays what it is; it used to change to the file's own folder, and your place was gone
    GalleryRemember();
    auto it = pathIndex_.find(ToLowerCopy(file));
    if (it != pathIndex_.end() && it->second >= 0 && it->second < static_cast<int>(entries.size()) && !entries[it->second].missing) {
        if (std::find(visibleOrder.begin(), visibleOrder.end(), it->second) == visibleOrder.end()) { // (a filter hides it: it is what was asked for)
            const int pixel = filters.pixel;
            filters = GalleryFilters{};
            filters.pixel = pixel;
            filterEpoch++;
        }
        SelectIndex(it->second);
        gridFindFrames = 6;
        return;
    }
    OpenFolder(fs::path(file).parent_path().wstring(), file); // selects the file once the scan arrives
}

void AppState::PlayVideo(int index) {
    if (index < 0 || index >= static_cast<int>(entries.size())) return;
    if (!videoPlayer) videoPlayer = std::make_unique<VideoPlayer>();
    ID3D11DeviceContext* ctx = nullptr;
    device_->GetImmediateContext(&ctx);
    const bool ok = videoPlayer->Open(device_, ctx, entries[index].path, videoVolume, videoMuted, videoLoop);
    if (ctx) ctx->Release();
    if (!ok) SetStatus(videoPlayer->Error());
}

void AppState::StopVideo() {
    if (videoPlayer) videoPlayer->Close();
}

bool AppState::UpdateVideo() {
    return videoPlayer && videoPlayer->IsOpen() && videoPlayer->Update();
}

void AppState::SelectIndex(int index, bool resetMultiSelect) {
    if (index < 0 || index >= static_cast<int>(entries.size())) return;
    StopVideo();

    ReleaseImage(currentTexture);
    selectedIndex = index;
    entries[index].arrivedTick = 0; // seen: no longer new
    zoom = 1.0f;
    fitToWindow = true;
    panX = panY = 0.0f;
    scrollToSelected = true;
    if (resetMultiSelect) {
        multiSelected.clear();
        multiSelected.insert(index);
        lastClickedIndex = index;
    }

    ImageEntry& entry = entries[index];

    std::string texError;
    if (!LoadImageTexture(device_, entry.path, currentTexture, &texError)) {
        SetStatus("Failed to load image: " + texError);
    }

    if (entry.thumbReady && !entry.metadata.slim) {
        // Metadata was already parsed by the background loader; reuse it
        // instead of hitting the disk again. (Slim copies lack the big raw fields: read the file.)
        currentMetadata = entry.metadata;
    } else {
        currentMetadata = ReadImageMetadata(entry.path);
    }
    if (!currentMetadata.error.empty()) {
        SetStatus("Metadata read error: " + currentMetadata.error);
    }
    if (videoAutoplay && currentMetadata.isVideo) PlayVideo(index);
}

void AppState::SelectRelative(int delta) {
    if (entries.empty()) return;
    // Follow the gallery's filtered order so arrows never land on a hidden image.
    const std::vector<int>& order = visibleOrder;
    if (!order.empty()) {
        int n = static_cast<int>(order.size());
        auto it = std::find(order.begin(), order.end(), selectedIndex);
        int pos = it == order.end() ? (delta >= 0 ? -1 : 0) : static_cast<int>(it - order.begin());
        int target = pos + delta;
        // Steps larger than one (Up/Down) stop at the ends; single steps wrap.
        if (std::abs(delta) > 1) target = std::clamp(target, 0, n - 1);
        else target = ((target % n) + n) % n;
        SelectIndex(order[target]);
        return;
    }
    int n = static_cast<int>(entries.size());
    SelectIndex(((selectedIndex + delta) % n + n) % n);
}

void AppState::SelectFirst() {
    if (!visibleOrder.empty()) SelectIndex(visibleOrder.front());
    else if (!entries.empty()) SelectIndex(0);
}

void AppState::SelectLast() {
    if (!visibleOrder.empty()) SelectIndex(visibleOrder.back());
    else if (!entries.empty()) SelectIndex(static_cast<int>(entries.size()) - 1);
}

void AppState::ReloadFolder(const std::wstring& selectPath) {
    if (currentFolder.empty()) return;
    std::wstring selectedPath = !selectPath.empty() ? selectPath
                                : (selectedIndex >= 0 && selectedIndex < static_cast<int>(entries.size()))
                                    ? entries[selectedIndex].path
                                    : std::wstring();
    OpenFolder(currentFolder, selectedPath);
}

void AppState::SetStatus(const std::string& msg) {
    statusMessage = msg;
    statusSetTick = GetTickCount64();
}

bool AppState::StatusActive() const {
    return !statusMessage.empty() && GetTickCount64() - statusSetTick < 8000;
}

void AppState::CancelScrape() {
    if (scrapeThread_.joinable()) {
        scrapeProgress_.cancelRequested = true;
        scrapeThread_.join();
    }
    scrapeProgress_.cancelRequested = false;
    scrapeRunning_ = false;
}

void AppState::StartScrapeFolder(const std::wstring& folder, bool recursive, ScrapeFormat format,
                                 const std::wstring& outputFile) {
    CancelScrape();
    scrapeProgress_.current = 0;
    scrapeProgress_.total = 0;
    scrapeRunning_ = true;
    lastScrapeKind_ = ScrapeKind::Folder;
    scrapeThread_ = std::thread([this, folder, recursive, format, outputFile]() {
        ScrapeResult r;
        try {
            r = ScrapeFolder(folder, recursive, format, outputFile, &scrapeProgress_);
        } catch (...) {
            r.message = "Unexpected error during scrape";
        }
        std::string msg = r.cancelled
            ? "Cancelled after " + std::to_string(r.filesScanned) + " file(s)."
            : (r.success ? ("Done: " + std::to_string(r.filesScanned) + " file(s) scanned, " +
                            std::to_string(r.filesWithMetadata) + " had metadata.")
                         : ("Failed: " + r.message));
        std::lock_guard<std::mutex> lock(scrapeResultMutex_);
        scrapeResultPending_ = std::move(msg);
        scrapeResultAvailable_ = true;
        scrapeRunning_ = false;
    });
}

void AppState::StartScrapeFiles(const std::vector<std::wstring>& files, ScrapeFormat format,
                                 const std::wstring& outputFile) {
    CancelScrape();
    scrapeProgress_.current = 0;
    scrapeProgress_.total = 0;
    scrapeRunning_ = true;
    lastScrapeKind_ = ScrapeKind::Selected;
    scrapeThread_ = std::thread([this, files, format, outputFile]() {
        ScrapeResult r;
        try {
            r = ScrapeFiles(files, format, outputFile, &scrapeProgress_);
        } catch (...) {
            r.message = "Unexpected error during export";
        }
        std::string msg = r.cancelled
            ? "Cancelled after " + std::to_string(r.filesScanned) + " file(s)."
            : (r.success ? ("Done: wrote " + std::to_string(r.filesScanned) + " entries.")
                         : ("Failed: " + r.message));
        std::lock_guard<std::mutex> lock(scrapeResultMutex_);
        scrapeResultPending_ = std::move(msg);
        scrapeResultAvailable_ = true;
        scrapeRunning_ = false;
    });
}

void AppState::PumpScrapeResult() {
    std::string msg;
    {
        std::lock_guard<std::mutex> lock(scrapeResultMutex_);
        if (!scrapeResultAvailable_) return;
        msg = std::move(scrapeResultPending_);
        scrapeResultAvailable_ = false;
    }
    if (lastScrapeKind_ == ScrapeKind::Selected) {
        exportSelectedResultMessage = std::move(msg);
    } else {
        scrapeResultMessage = std::move(msg);
    }
}

// ---------------------------------------------------------------------------
// NSFW classification / sorting
// ---------------------------------------------------------------------------

float EffectiveNsfwScore(const ImageEntry& e, NsfwSource source) {
    switch (source) {
        case NsfwSource::ImageModel: return e.nsfwImageScore;
        case NsfwSource::Prompt: return e.nsfwPromptScore;
        case NsfwSource::Combined:
        default: return std::max(e.nsfwImageScore, e.nsfwPromptScore);
    }
}

bool AppState::ShouldBlur(const ImageEntry& e) {
    if (!blurNsfw || nsfwRevealed.count(ToLowerCopy(e.path))) return false;
    const int mark = NsfwMark(e.path);
    if (mark >= 0) return mark == 1;
    const float s = EffectiveNsfwScore(e, nsfwSource);
    return s >= 0.0f && s >= nsfwThreshold;
}

bool AppState::ShouldBlur(const std::wstring& path) {
    if (!blurNsfw || path.empty()) return false;
    const std::wstring low = ToLowerCopy(path);
    if (nsfwRevealed.count(low)) return false;
    // pose drawings (the library's mannequins, Generate's trimmed copies): bare figures the blur turned into a white page - never blurred
    if (low.find(L"\\libraries\\poses\\") != std::wstring::npos || low.find(L"\\generate\\poses\\") != std::wstring::npos) return false;
    return IsNsfw(path);
}

void AppState::NoteFileDeleted(const std::wstring& path) {
    auto it = pathIndex_.find(ToLowerCopy(path));
    if (it == pathIndex_.end() || it->second < 0 || it->second >= static_cast<int>(entries.size()) || entries[it->second].missing) return;
    entries[it->second].missing = true;
    missingCount_++;
    filterEpoch++;
}

bool AppState::IsNsfw(const std::wstring& path) {
    if (path.empty()) return false;
    const int mark = NsfwMark(path);
    if (mark >= 0) return mark == 1;
    const std::wstring key = ToLowerCopy(path);
    auto at = pathIndex_.find(key);
    if (at != pathIndex_.end() && at->second >= 0 && at->second < static_cast<int>(entries.size())) {
        const float s = EffectiveNsfwScore(entries[at->second], nsfwSource);
        if (s >= 0.0f) return s >= nsfwThreshold;
    }
    const bool inFolder = at != pathIndex_.end(); // (not scored yet: the folder's own scoring does those)
    {
        std::lock_guard<std::mutex> lock(nsfwCacheMutex_); // a picture outside the open folder: its classified score, if it has one
        auto c = nsfwCache_.find(key);
        if (c != nsfwCache_.end() && c->second.score >= 0.0f) return c->second.score >= nsfwThreshold;
        if (inFolder || nsfwUnscorable_.count(key)) return false;
    }
    // never scored: scored in the background, blurred until then
    if (nsfwAsked_.insert(key).second) nsfwAskQueue_.push_back(path);
    PumpNsfwAsks();
    return true;
}

ID3D11ShaderResourceView* AppState::ShownSrv(ID3D11ShaderResourceView* srv, bool blur) {
    return blur && srv ? BlurredCopy(device_, srv) : srv;
}

namespace {


std::wstring AppDataDir() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = (len > 0 && len < MAX_PATH) ? buf : L".";
    return base + L"\\Try2Create";
}

std::wstring NsfwCachePath() { return AppDataDir() + L"\\cache\\nsfw_scores.json"; }
std::wstring OrganizeSettingsPath() { return AppDataDir() + L"\\organize_settings.json"; }

int64_t MtimeTicks(fs::file_time_type t) { return static_cast<int64_t>(t.time_since_epoch().count()); }


} // namespace

void AppState::LoadNsfwCache() {
    nsfwCacheModelId_ = NsfwClassifier::ModelId();
    std::ifstream f(NsfwCachePath(), std::ios::binary);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    // Scores from a different model aren't comparable; start over.
    if (!j.is_object() || j.value("model", std::string()) != nsfwCacheModelId_) return;
    if (!j.contains("entries") || !j["entries"].is_array()) return;
    std::lock_guard<std::mutex> lock(nsfwCacheMutex_);
    for (auto& e : j["entries"]) {
        if (!e.is_object() || !e.contains("path") || !e["path"].is_string()) continue;
        CachedNsfw c;
        c.mtime = e.value("mtime", int64_t{0});
        c.size = e.value("size", uintmax_t{0});
        c.score = e.value("score", -1.0f);
        std::string path = e["path"].get<std::string>();
        int len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (len <= 1) continue;
        std::wstring wpath(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), len);
        nsfwCache_[wpath] = c;
    }
}

void AppState::SaveNsfwCache() {
    nlohmann::json j;
    j["model"] = nsfwCacheModelId_;
    nlohmann::json arr = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(nsfwCacheMutex_);
        for (auto& [path, c] : nsfwCache_) {
            arr.push_back({{"path", NarrowUtf8(path)}, {"mtime", c.mtime}, {"size", c.size}, {"score", c.score}});
        }
    }
    j["entries"] = std::move(arr);
    std::wstring path = NsfwCachePath();
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << j.dump();
    }
    fs::rename(tmp, path, ec);
}

int AppState::NsfwMark(const std::wstring& path) {
    if (!nsfwMarksLoaded_) {
        nsfwMarksLoaded_ = true;
        std::ifstream f(AppDataDir() + L"\\nsfw_marks.json", std::ios::binary);
        const nlohmann::json j = f ? nlohmann::json::parse(f, nullptr, false) : nlohmann::json();
        if (j.is_object())
            for (auto it = j.begin(); it != j.end(); ++it)
                if (it.value().is_number_integer()) nsfwMarks_[ToLowerCopy(WidenUtf8(it.key()))] = it.value().get<int>() ? 1 : 0;
    }
    auto it = nsfwMarks_.find(ToLowerCopy(path));
    return it == nsfwMarks_.end() ? -1 : it->second;
}

void AppState::SetNsfwMark(const std::wstring& path, int mark) {
    NsfwMark(path); // (loaded)
    const std::wstring key = ToLowerCopy(path);
    if (mark < 0) nsfwMarks_.erase(key);
    else nsfwMarks_[key] = mark ? 1 : 0;
    if (mark == 1) nsfwRevealed.erase(key); // (marked NSFW: blurred at once)
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [p, m] : nsfwMarks_) j[NarrowUtf8(p)] = m;
    const std::string text = j.dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::ofstream(AppDataDir() + L"\\nsfw_marks.json", std::ios::binary | std::ios::trunc) << text;
}

void AppState::PumpNsfwAsks() {
    if (nsfwAskFut_.valid() && nsfwAskFut_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            nsfwAskFut_.get();
        } catch (...) {
        }
    }
    if (nsfwAskFut_.valid() || nsfwAskQueue_.empty()) return;
    std::vector<std::wstring> batch;
    while (!nsfwAskQueue_.empty() && batch.size() < 24) batch.push_back(nsfwAskQueue_.front()), nsfwAskQueue_.pop_front();
    nsfwAskFut_ = std::async(std::launch::async, [this, batch] {
        std::string err;
        const bool ok = nsfwClassifier_.EnsureLoaded(&err); // (no classifier: none of them can be scored, none stays blurred for ever)
        bool scored = false;
        for (const std::wstring& p : batch) {
            if (nsfwAskStop_) break;
            float score = -1.0f;
            std::error_code ec;
            const auto mtime = fs::last_write_time(p, ec);
            const uintmax_t size = ec ? 0 : fs::file_size(p, ec);
            if (ok) {
                try {
                    score = nsfwClassifier_.ScoreFile(p, nullptr);
                } catch (...) {
                }
            }
            std::wstring ext = fs::path(p).extension().wstring();
            for (wchar_t& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
            const bool video = ext == L".mp4" || ext == L".webm" || ext == L".mov" || ext == L".mkv" || ext == L".avi" || ext == L".m4v";
            if (ok && score < 0.0f && video && !FfmpegPath().empty()) { // a video: two of its frames (the most explicit counts)
                wchar_t tmp[MAX_PATH] = {};
                GetTempPathW(MAX_PATH, tmp);
                const std::wstring still = std::wstring(tmp) + L"try2create_nsfw_" + std::to_wstring(GetCurrentThreadId()) + L".png";
                for (const wchar_t* at : {L"0.5", L"2.5"}) {
                    std::string log;
                    std::error_code ec2;
                    fs::remove(still, ec2);
                    RunCaptureHidden(L"\"" + FfmpegPath() + L"\" -y -hide_banner -loglevel error -ss " + at + L" -i \"" + p + L"\" -frames:v 1 -vf scale=512:-2 \"" + still + L"\"", log, true);
                    if (!fs::exists(still, ec2)) continue;
                    try {
                        score = std::max(score, nsfwClassifier_.ScoreFile(still, nullptr));
                    } catch (...) {
                    }
                }
                std::error_code ec3;
                fs::remove(still, ec3);
            }
            std::lock_guard<std::mutex> lock(nsfwCacheMutex_);
            if (score >= 0.0f) nsfwCache_[ToLowerCopy(p)] = {MtimeTicks(mtime), size, score}, scored = true;
            else nsfwUnscorable_.insert(ToLowerCopy(p));
        }
        if (scored) {
            try {
                SaveNsfwCache();
            } catch (...) {
            }
        }
    });
}

float AppState::LookupNsfwCache(const std::wstring& path, fs::file_time_type mtime, uintmax_t size) {
    std::lock_guard<std::mutex> lock(nsfwCacheMutex_);
    auto it = nsfwCache_.find(ToLowerCopy(path));
    if (it == nsfwCache_.end() || it->second.mtime != MtimeTicks(mtime) || it->second.size != size) return -1.0f;
    return it->second.score;
}

const char* AppState::NsfwJobPhase() const {
    switch (nsfwProgress_.phase.load()) {
        case kOrgScanning: return "Scanning folders";
        case kOrgLoadingModel: return "Loading model (first run can take a few seconds)...";
        case kOrgScoring: return "Classifying";
        case kOrgDuplicates: return "Checking for duplicates";
        case kOrgMoving: return "Moving files";
        case kOrgCleaning: return "Removing empty folders";
        case kOrgUndoing: return "Undoing";
        default: return "";
    }
}

void AppState::CancelNsfwJob() {
    if (nsfwThread_.joinable()) {
        nsfwProgress_.cancel = true;
        nsfwThread_.join();
    }
    nsfwProgress_.cancel = false;
    nsfwRunning_ = false;
    nsfwProgress_.phase = kOrgIdle;
    // Leftover results are still delivered by PumpNsfwResults: scores are
    // matched by path (harmless after a folder change) and a cancelled
    // organize still needs its rescan for the files it already moved.
}

void AppState::LaunchNsfwThread(std::function<OrganizeResult()> job) {
    CancelNsfwJob();
    nsfwProgress_.current = 0;
    nsfwProgress_.total = 0;
    nsfwRunning_ = true;
    nsfwThread_ = std::thread([this, job = std::move(job)]() {
        OrganizeResult r;
        try {
            r = job();
        } catch (...) {
            r.message = "Unexpected error while processing files.";
        }
        try {
            SaveNsfwCache();
        } catch (...) {
            // Losing score persistence just means reclassifying next time.
        }
        std::lock_guard<std::mutex> lock(nsfwResultsMutex_);
        nsfwFinishedMessage_ = std::move(r.message);
        nsfwNeedsRescan_ = r.filesChanged;
        nsfwFinished_ = true;
        nsfwProgress_.phase = kOrgIdle;
        nsfwRunning_ = false;
    });
}

OrganizeHooks AppState::MakeOrganizeHooks() {
    OrganizeHooks hooks;
    hooks.cachedScore = [this](const std::wstring& path, fs::file_time_type mtime, uintmax_t size) {
        return LookupNsfwCache(path, mtime, size);
    };
    hooks.storeScore = [this](const std::wstring& path, fs::file_time_type mtime, uintmax_t size, float score) {
        std::lock_guard<std::mutex> lock(nsfwCacheMutex_);
        nsfwCache_[ToLowerCopy(path)] = {MtimeTicks(mtime), size, score};
    };
    hooks.renameScore = [this](const std::wstring& from, const std::wstring& to) {
        std::lock_guard<std::mutex> lock(nsfwCacheMutex_);
        auto it = nsfwCache_.find(ToLowerCopy(from));
        if (it == nsfwCache_.end()) return;
        CachedNsfw c = it->second;
        nsfwCache_.erase(it);
        // A cross-volume move rewrites the file; re-read its timestamp so the
        // entry still matches.
        std::error_code ec;
        auto t = fs::last_write_time(to, ec);
        if (!ec) c.mtime = MtimeTicks(t);
        nsfwCache_[ToLowerCopy(to)] = c;
    };
    hooks.renameScore = [prev = hooks.renameScore, this](const std::wstring& from, const std::wstring& to) {
        {
            std::lock_guard<std::mutex> lock(ratingsMutex_);
            auto it = ratings_.find(ToLowerCopy(from));
            if (it != ratings_.end()) {
                int stars = it->second;
                ratings_.erase(it);
                ratings_[ToLowerCopy(to)] = stars;
                ratingsDirty_ = true;
                ratingsChangedTick_ = GetTickCount64();
            }
        }
        if (prev) prev(from, to);
    };
    return hooks;
}

void AppState::StartNsfwScoreJob(const std::vector<int>& indices) {
    struct Item {
        size_t index;
        std::wstring path;
        fs::file_time_type mtime;
        uintmax_t size;
    };
    std::vector<Item> items;
    for (int idx : indices) {
        if (idx < 0 || idx >= static_cast<int>(entries.size())) continue;
        const ImageEntry& e = entries[idx];
        if (e.nsfwImageScore >= 0.0f) continue; // already scored (or cached)
        items.push_back({static_cast<size_t>(idx), e.path, e.mtime, e.size});
    }
    LaunchNsfwThread([this, items = std::move(items)]() {
        OrganizeResult r;
        nsfwProgress_.total = static_cast<int>(items.size());
        if (items.empty()) {
            r.message = "Everything here is already scored.";
            return r;
        }
        nsfwProgress_.phase = kOrgLoadingModel;
        std::string err;
        if (!nsfwClassifier_.EnsureLoaded(&err)) {
            r.message = "Image classifier unavailable: " + err;
            return r;
        }
        nsfwProgress_.phase = kOrgScoring;
        std::atomic<size_t> next{0};
        std::atomic<int> failed{0};
        auto worker = [&]() {
            for (size_t i = next++; i < items.size() && !nsfwProgress_.cancel.load(); i = next++) {
                const Item& it = items[i];
                float score = -1.0f;
                try {
                    score = nsfwClassifier_.ScoreFile(it.path, nullptr);
                } catch (...) {
                }
                if (score >= 0) {
                    std::lock_guard<std::mutex> lock(nsfwCacheMutex_);
                    nsfwCache_[ToLowerCopy(it.path)] = {MtimeTicks(it.mtime), it.size, score};
                } else {
                    failed++;
                }
                {
                    std::lock_guard<std::mutex> lock(nsfwResultsMutex_);
                    nsfwPendingScores_.push_back({it.index, it.path, score});
                }
                nsfwProgress_.current++;
            }
        };
        // Decoding multi-megapixel PNGs dominates, so decode on several
        // threads; inference itself is serialized inside the classifier.
        unsigned threadCount = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threadCount; ++t) pool.emplace_back(worker);
        worker();
        for (auto& t : pool) t.join();
        r.message = std::string(nsfwProgress_.cancel ? "Cancelled. " : "") + "Scored " +
                    std::to_string(nsfwProgress_.current.load()) + " image(s)." +
                    (failed ? " " + std::to_string(failed.load()) + " could not be decoded/classified." : std::string());
        return r;
    });
}

void AppState::StartOrganize(const OrganizeOptions& opts) {
    SaveOrganizeSettings();
    OrganizeHooks hooks = MakeOrganizeHooks();
    LaunchNsfwThread([this, opts, hooks]() { return RunOrganize(opts, nsfwClassifier_, hooks, nsfwProgress_); });
}

void AppState::StartUndoOrganize() {
    std::wstring logPath = LatestOrganizeLog();
    OrganizeHooks hooks = MakeOrganizeHooks();
    LaunchNsfwThread([this, logPath, hooks]() { return UndoOrganize(logPath, hooks, nsfwProgress_); });
}

OrganizeOptions AppState::MakeOrganizeOptions(const std::wstring& source, const std::wstring& dest,
                                              bool dryRun) const {
    OrganizeOptions o;
    o.sourceRoot = source;
    o.destRoot = dest;
    // Comma/semicolon-separated folder names, relative to the source.
    std::string cur;
    auto flush = [&]() {
        size_t a = cur.find_first_not_of(" \t"), b = cur.find_last_not_of(" \t");
        if (a != std::string::npos) {
            std::string t = cur.substr(a, b - a + 1);
            int len = MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, nullptr, 0);
            std::wstring w(len > 0 ? len - 1 : 0, L'\0');
            if (len > 0) MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, w.data(), len);
            o.excludes.push_back(w);
        }
        cur.clear();
    };
    for (char c : organizeExcludes) {
        if (c == ',' || c == ';') flush();
        else cur += c;
    }
    flush();
    o.source = nsfwSource;
    o.threshold = nsfwThreshold;
    o.useQuestionable = nsfwUseQuestionable;
    o.questionableLow = nsfwQuestionableLow;
    o.includeVideos = organizeVideos;
    o.moveOtherFiles = organizeOtherFiles;
    o.copyInsteadOfMove = organizeCopy;
    o.removeEmptyFolders = organizeRemoveEmpty;
    o.keepSubfolders = organizeKeepSubfolders;
    o.detectDuplicates = organizeDuplicates;
    o.dryRun = dryRun;
    return o;
}

void AppState::LoadOrganizeSettings() {
    std::ifstream f(OrganizeSettingsPath(), std::ios::binary);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return;
    organizeSource = j.value("source", organizeSource);
    organizeDest = j.value("dest", organizeDest);
    organizeExcludes = j.value("excludes", organizeExcludes);
    organizeVideos = j.value("videos", organizeVideos);
    organizeOtherFiles = j.value("otherFiles", organizeOtherFiles);
    organizeRemoveEmpty = j.value("removeEmpty", organizeRemoveEmpty);
    organizeCopy = j.value("copy", organizeCopy);
    organizeKeepSubfolders = j.value("keepSubfolders", organizeKeepSubfolders);
    organizeDuplicates = j.value("duplicates", organizeDuplicates);
    // Older settings files have no collection yet; the last destination is
    // the natural default.
    organizeCollection = j.value("collection", organizeDest);
    nsfwThreshold = j.value("threshold", nsfwThreshold);
    nsfwUseQuestionable = j.value("useQuestionable", nsfwUseQuestionable);
    nsfwQuestionableLow = j.value("questionableLow", nsfwQuestionableLow);
    nsfwSource = static_cast<NsfwSource>(std::clamp(j.value("scoreSource", 0), 0, 2));
}

void AppState::SaveOrganizeSettings() {
    nlohmann::json j = {
        {"source", organizeSource},
        {"dest", organizeDest},
        {"excludes", organizeExcludes},
        {"videos", organizeVideos},
        {"otherFiles", organizeOtherFiles},
        {"removeEmpty", organizeRemoveEmpty},
        {"copy", organizeCopy},
        {"keepSubfolders", organizeKeepSubfolders},
        {"duplicates", organizeDuplicates},
        {"collection", organizeCollection},
        {"threshold", nsfwThreshold},
        {"useQuestionable", nsfwUseQuestionable},
        {"questionableLow", nsfwQuestionableLow},
        {"scoreSource", static_cast<int>(nsfwSource)},
    };
    std::error_code ec;
    fs::create_directories(AppDataDir(), ec);
    std::ofstream out(OrganizeSettingsPath(), std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2);
}

void AppState::PumpNsfwResults() {
    std::vector<NsfwScoreResult> scores;
    bool finished = false, rescan = false;
    std::string msg;
    {
        std::lock_guard<std::mutex> lock(nsfwResultsMutex_);
        scores.swap(nsfwPendingScores_);
        if (nsfwFinished_) {
            finished = true;
            rescan = nsfwNeedsRescan_;
            msg = std::move(nsfwFinishedMessage_);
            nsfwFinished_ = false;
            nsfwNeedsRescan_ = false;
        }
    }
    if (!scores.empty()) filterEpoch++;
    for (auto& r : scores) {
        if (r.index < entries.size() && entries[r.index].path == r.path && !entries[r.index].clefGraded) entries[r.index].nsfwImageScore = r.imageScore;
    }
    if (!finished) return;
    nsfwResultMessage = msg;
    SetStatus(msg.substr(0, msg.find('\n'))); // the toolbar is one line; NSFW Sort shows the full summary
    if (rescan && !currentFolder.empty()) {
        std::wstring folder = currentFolder;
        OpenFolder(folder); // files moved: re-list (the caches make this fast)
    }
}

// ---------------------------------------------------------------------------
// Vision prompts
// ---------------------------------------------------------------------------

namespace {
std::wstring VisionSettingsPath() { return AppDataDir() + L"\\vision_settings.json"; }
} // namespace

AppState::VisionPlan AppState::MakeVisionPlan() const {
    VisionPlan plan;
    plan.instruction = visionInstruction;
    plan.options.mode = VisionModeKey();
    plan.options.txtSuffix = VisionTxtSuffix();
    switch (visionMode) {
        case VisionMode::Tags:
            plan.instruction = visionTagsInstruction.empty() ? DefaultTagsInstruction() : visionTagsInstruction;
            plan.options.numPredict = 500;
            plan.options.temperature = 0.2;
            break;
        case VisionMode::VideoScene:
            plan.instruction = BuildVideoInstruction(videoOptions);
            plan.options.numPredict = VideoTokenBudget(videoOptions.seconds);
            plan.options.temperature = 0.7; // a little freer: this is creative writing, not transcription
            break;
        case VisionMode::ImagePrompt:
        default:
            if (!StyleDirective().empty()) {
                plan.instruction = BuildStyledInstruction(visionInstruction.empty() ? DefaultVisionInstruction() : visionInstruction, StyleDirective());
                plan.options.temperature = 0.6; // restyling is creative rewriting
            }
            break;
    }
    if (visionAutoSave && !visionOutputDir.empty()) plan.saveDir = WidenUtf8(visionOutputDir);
    return plan;
}

void AppState::DescribeWithVision(const std::vector<int>& indices) {
    std::vector<std::wstring> paths;
    for (int idx : indices) {
        if (idx >= 0 && idx < static_cast<int>(entries.size()) && !entries[idx].missing) paths.push_back(entries[idx].path);
    }
    DescribePaths(paths);
}

std::wstring AppState::SaveDirFor(const std::wstring& imagePath, const std::wstring& batchDir) const {
    if (batchDir.empty()) return batchDir;
    std::wstring root = batchDir;
    while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    const std::wstring parent = fs::path(imagePath).parent_path().wstring();
    if (parent.size() >= root.size() && _wcsnicmp(parent.c_str(), root.c_str(), root.size()) == 0 &&
        (parent.size() == root.size() || parent[root.size()] == L'\\' || parent[root.size()] == L'/')) {
        return parent; // already in the output folder (maybe in a subfolder): describe it where it is
    }
    return batchDir;
}

void AppState::DescribePaths(const std::vector<std::wstring>& paths) {
    VisionPlan plan = MakeVisionPlan();
    const bool skipDone = visionSkipDone && paths.size() > 1 && !plan.saveDir.empty();
    std::vector<VisionService::Item> items;
    int skipped = 0;
    for (const std::wstring& path : paths) {
        const std::wstring into = SaveDirFor(path, plan.saveDir);
        if (skipDone && HasSavedResult(path, into, plan.options.txtSuffix)) {
            ++skipped;
            continue;
        }
        VisionService::Item item;
        item.path = path;
        if (into != plan.saveDir) item.saveDir = into;
        if (visionUseTags) {
            // the image's own prompt as a hint: from the gallery when it is there, else read from the file
            auto found = pathIndex_.find(ToLowerCopy(path));
            if (found != pathIndex_.end() && found->second < static_cast<int>(entries.size()) && entries[found->second].thumbReady) {
                item.tags = entries[found->second].metadata.parsed.positivePrompt;
            } else {
                item.readTags = true;
            }
        }
        items.push_back(std::move(item));
    }
    visionMessage = skipped ? "Skipped " + std::to_string(skipped) + " image(s) that already have a result in the output folder." : "";
    if (items.empty()) {
        if (skipped) visionMessage += " Nothing left to describe.";
        return;
    }
    visionBatchSaved_ = visionBatchFailed_ = 0;
    SaveVisionSettings();
    vision.Describe(VisionEndpoint(), visionModel, visionKeepAlive, plan.instruction, std::move(items), plan.saveDir, plan.options);
}

void AppState::CancelVisionFolder() {
    visionFolderCancel_ = true;
    vision.Cancel();
    if (visionFolderThread_.joinable()) visionFolderThread_.join();
    visionFolderCancel_ = false;
    visionFolderBusy_ = false;
}

void AppState::StartVisionFolder(const std::wstring& folder, bool recursive) {
    CancelVisionFolder();
    VisionPlan plan = MakeVisionPlan();
    const bool skipDone = visionSkipDone && !plan.saveDir.empty();
    const bool useTags = visionUseTags;
    const std::wstring excludeDir = hideVisionFolder ? plan.saveDir : std::wstring();
    const std::string model = visionModel, keepAlive = visionKeepAlive;
    visionMessage = "Scanning the folder...";
    visionBatchSaved_ = visionBatchFailed_ = 0;
    SaveVisionSettings();
    visionFolderBusy_ = true;
    visionFolderThread_ = std::thread([this, folder, recursive, plan, skipDone, useTags, excludeDir, model, keepAlive]() {
        std::string message;
        try {
            std::vector<ListedImage> files = ListImageFiles(folder, recursive, excludeDir);
            const int found = static_cast<int>(files.size());
            const int copies = DropDuplicateCopies(files, plan.saveDir);
            std::vector<VisionService::Item> items;
            int skipped = 0;
            for (const ListedImage& f : files) {
                if (visionFolderCancel_.load()) break;
                if (skipDone && HasSavedResult(f.path, plan.saveDir, plan.options.txtSuffix)) {
                    ++skipped;
                    continue;
                }
                VisionService::Item item;
                item.path = f.path;
                item.readTags = useTags;
                items.push_back(std::move(item));
            }
            if (visionFolderCancel_.load()) {
                message = "Cancelled.";
            } else if (items.empty()) {
                message = found == 0 ? "No images found in that folder."
                                     : "Nothing to describe: all " + std::to_string(found - copies) + " image(s) already have a result.";
            } else {
                message = "Describing " + std::to_string(items.size()) + " image(s)" +
                          (skipped ? ", " + std::to_string(skipped) + " already done" : std::string()) +
                          (copies ? ", " + std::to_string(copies) + " duplicate cop" + (copies == 1 ? "y" : "ies") + " skipped" : std::string()) + ".";
                vision.Describe(VisionEndpoint(), model, keepAlive, plan.instruction, std::move(items), plan.saveDir, plan.options);
            }
        } catch (...) {
            message = "Could not read that folder.";
        }
        {
            std::lock_guard<std::mutex> lock(visionFolderMutex_);
            visionFolderMessage_ = std::move(message);
        }
        visionFolderBusy_ = false;
    });
}

void AppState::PumpVisionResults() {
    {
        std::lock_guard<std::mutex> lock(visionFolderMutex_);
        if (!visionFolderMessage_.empty()) {
            visionMessage = std::move(visionFolderMessage_);
            visionFolderMessage_.clear();
            SetStatus(visionMessage);
        }
    }
    std::vector<VisionService::Result> results = vision.TakeResults();
    if (results.empty()) return;
    bool anyLibrary = false;
    for (auto& r : results) {
        if (r.mode == "compose") { // not a library result: the Vision board asked for it
            composeResults.push_back(std::move(r));
            continue;
        }
        anyLibrary = true;
        if (r.text.empty()) {
            ++visionBatchFailed_;
            continue;
        }
        visionTexts[VisionTextKey(r.path, r.mode)] = r.text;
        {
            const int bit = r.mode.empty() ? 1 : (r.mode == "tags" ? 2 : 4);
            const std::wstring key = ToLowerCopy(r.path);
            sessionMask_[key] |= bit;
            auto pit = pathIndex_.find(key);
            if (pit != pathIndex_.end() && pit->second < static_cast<int>(entries.size())) entries[pit->second].visionMask |= bit;
            filterEpoch++;
        }
        if (!r.savedPath.empty()) {
            ++visionBatchSaved_;
            MarkDescribedDirty();
        }
        if (!r.error.empty()) ++visionBatchFailed_;
    }
    if (!anyLibrary) return;
    std::string msg;
    if (visionBatchSaved_) msg = "Saved " + std::to_string(visionBatchSaved_) + " image(s) + .txt to the output folder.";
    else msg = "Done.";
    if (visionBatchFailed_) msg += " " + std::to_string(visionBatchFailed_) + " failed (see the Vision Prompt window).";
    visionMessage = msg;
    SetStatus(msg);
}

void AppState::LoadVisionSettings() {
    std::ifstream f(VisionSettingsPath(), std::ios::binary);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return;
    visionModel = j.value("model", visionModel);
    visionKeepAlive = j.value("keepAlive", visionKeepAlive);
    visionOutputDir = j.value("outputDir", visionOutputDir);
    visionAutoSave = j.value("autoSave", visionAutoSave);
    visionUseTags = j.value("useTags", visionUseTags);
    visionStopOnExit = j.value("stopOnExit", visionStopOnExit);
    visionSkipDone = j.value("skipDone", visionSkipDone);
    visionStyle = j.value("style", visionStyle);
    visionStyleCustom = j.value("styleCustom", visionStyleCustom);
    visionWarnBig = j.value("warnBig", visionWarnBig);
    visionWarnOver = std::clamp(j.value("warnOver", visionWarnOver), 2, 100000);
    vision.SetStopOnExit(visionStopOnExit);
    visionInstruction = j.value("instruction", visionInstruction);
    visionTagsInstruction = j.value("tagsInstruction", visionTagsInstruction);
    visionMode = static_cast<VisionMode>(std::clamp(j.value("mode", 0), 0, 2));
    if (j.contains("video") && j["video"].is_object()) {
        const auto& v = j["video"];
        videoOptions.seconds = std::clamp(v.value("seconds", videoOptions.seconds), 5, 30);
        videoOptions.timeline = v.value("timeline", videoOptions.timeline);
        videoOptions.camera = std::clamp(v.value("camera", videoOptions.camera), 0, VideoCameraCount() - 1);
        videoOptions.sound = v.value("sound", videoOptions.sound);
        videoOptions.extra = v.value("extra", videoOptions.extra);
        videoOptions.templateText = v.value("template", videoOptions.templateText);
    }
}

void AppState::SaveVisionSettings() {
    MarkDescribedDirty();
    vision.SetStopOnExit(visionStopOnExit);
    // A prompt equal to the built-in one is stored as "" so it keeps following the built-in
    // wording; only text the user actually changed is pinned.
    auto pinned = [](const std::string& text, const char* builtin) { return text == builtin ? std::string() : text; };
    nlohmann::json j = {
        {"model", visionModel},     {"keepAlive", visionKeepAlive}, {"outputDir", visionOutputDir},
        {"autoSave", visionAutoSave}, {"useTags", visionUseTags}, {"stopOnExit", visionStopOnExit}, {"skipDone", visionSkipDone}, {"style", visionStyle}, {"styleCustom", visionStyleCustom}, {"warnBig", visionWarnBig}, {"warnOver", visionWarnOver},
        {"instruction", pinned(visionInstruction, DefaultVisionInstruction())},
        {"tagsInstruction", pinned(visionTagsInstruction, DefaultTagsInstruction())}, {"mode", static_cast<int>(visionMode)},
        {"video", {{"seconds", videoOptions.seconds}, {"timeline", videoOptions.timeline}, {"camera", videoOptions.camera},
                   {"sound", videoOptions.sound}, {"extra", videoOptions.extra},
                   {"template", pinned(videoOptions.templateText, DefaultVideoTemplate())}}},
    };
    std::error_code ec;
    fs::create_directories(AppDataDir(), ec);
    std::ofstream out(VisionSettingsPath(), std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2);
}

// ---------------------------------------------------------------------------
// UI preferences
// ---------------------------------------------------------------------------

namespace {
std::wstring UiSettingsPath() { return AppDataDir() + L"\\ui_settings.json"; }
} // namespace

void AppState::AddRecentFolder(const std::wstring& folder) {
    std::string u8 = NarrowUtf8(folder);
    if (u8.empty()) return;
    recentFolders.erase(std::remove_if(recentFolders.begin(), recentFolders.end(),
                                       [&](const std::string& r) { return _stricmp(r.c_str(), u8.c_str()) == 0; }),
                        recentFolders.end());
    recentFolders.insert(recentFolders.begin(), u8);
    if (recentFolders.size() > 10) recentFolders.resize(10);
    SaveUiSettings(); // so the list survives even if the app is killed later
}

void AppState::LoadUiSettings() {
    std::ifstream f(UiSettingsPath(), std::ios::binary);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return;
    thumbnailSize = std::clamp(j.value("thumbnailSize", thumbnailSize), 60.0f, 220.0f);
    sortMode = static_cast<SortMode>(std::clamp(j.value("sortMode", 0), 0, 10));
    sortDescending = j.value("sortDescending", sortDescending);
    metadataMode = j.contains("metadataMode") ? std::clamp(j.value("metadataMode", 1), 0, 2) : (j.value("onlyWithMetadata", true) ? 1 : 0);
    showUnreadable = j.value("showUnreadable", showUnreadable);
    showQualityBadges = j.value("showQualityBadges", showQualityBadges);
    showFolderPane = j.value("showFolderPane", showFolderPane);
    navCollapsed = j.value("navCollapsed", navCollapsed);
    tilePages = j.value("tilePages", tilePages);
    galleryHideViewer = j.value("galleryHideViewer", galleryHideViewer);
    galleryHideInfo = j.value("galleryHideInfo", galleryHideInfo);
    visionLibBoxes = j.value("visionLibBoxes", visionLibBoxes);
    requestTab = std::clamp(j.value("lastTab", 0), 0, 12); // opens on the tab you were using
    requestAlone = true;                                   // (on its own)
    visionLibFolders = j.value("visionLibFolders", visionLibFolders);
    visionLibBoxSize = std::clamp(j.value("visionLibBoxSize", visionLibBoxSize), 80.0f, 280.0f);
    visionLibImageFrac = std::clamp(j.value("visionLibImageFrac", visionLibImageFrac), 0.12f, 0.88f);
    visionLibSideBySide = j.value("visionLibSideBySide", visionLibSideBySide);
    showNsfwBadges = j.value("showNsfwBadges", showNsfwBadges);
    blurNsfw = j.value("blurNsfw", blurNsfw);
    showHelpers = j.value("showHelpers", showHelpers);
    charView = std::clamp(j.value("charView", charView), 0, 2);
    charTile = std::clamp(j.value("charTile", charTile), 60.0f, 260.0f);
    watchFolder = j.value("watchFolder", watchFolder);
    hideVisionFolder = j.value("hideVisionFolder", hideVisionFolder);
    hideDuplicateCopies = j.value("hideDuplicateCopies", hideDuplicateCopies);
    followNewest = j.value("followNewest", followNewest);
    scanSubfolders = j.value("scanSubfolders", scanSubfolders);
    showGroupHeaders = j.value("showGroupHeaders", showGroupHeaders);
    filters.pixel = std::clamp(j.value("pixelFilter", 0), 0, 2);
    videoVolume = std::clamp(j.value("videoVolume", videoVolume), 0.0f, 1.0f);
    VideoPlayer::SetVideoVolume(videoVolume);
    videoMuted = j.value("videoMuted", videoMuted);
    videoLoop = j.value("videoLoop", videoLoop);
    videoAutoplay = j.value("videoAutoplay", videoAutoplay);
    lightTheme = j.value("lightTheme", lightTheme);
    accent = j.value("accent", accent);
    uiScaleMul = std::clamp(j.value("uiScale", uiScaleMul), 0.7f, 1.8f);
    slideshowSeconds = std::clamp(j.value("slideshowSeconds", slideshowSeconds), 1.0f, 60.0f);
    if (j.contains("savedSearches") && j["savedSearches"].is_array()) {
        for (auto& s : j["savedSearches"]) {
            if (s.is_object() && s.value("query", std::string()).size() > 0) {
                savedSearches.push_back({s.value("name", s.value("query", std::string())), s.value("query", std::string())});
            }
        }
    }
    if (j.contains("searchHistory") && j["searchHistory"].is_array()) {
        for (auto& s : j["searchHistory"]) {
            if (s.is_string() && searchHistory.size() < 15) searchHistory.push_back(s.get<std::string>());
        }
    }
    if (j.contains("recentFolders") && j["recentFolders"].is_array()) {
        for (auto& r : j["recentFolders"]) {
            if (r.is_string() && recentFolders.size() < 10) recentFolders.push_back(r.get<std::string>());
        }
    }
    if (j.contains("window") && j["window"].is_object()) {
        const auto& w = j["window"];
        winX = w.value("x", 0);
        winY = w.value("y", 0);
        winW = w.value("w", 0);
        winH = w.value("h", 0);
        winMaximized = w.value("maximized", false);
        hasWinPlacement = winW > 200 && winH > 200;
    }
}

void AppState::SaveUiSettings() {
    nlohmann::json j = {
        {"thumbnailSize", thumbnailSize},
        {"sortMode", static_cast<int>(sortMode)},
        {"sortDescending", sortDescending},
        {"metadataMode", metadataMode},
        {"showUnreadable", showUnreadable},
        {"showQualityBadges", showQualityBadges},
        {"showFolderPane", showFolderPane},
        {"navCollapsed", navCollapsed},
        {"tilePages", tilePages},
        {"galleryHideViewer", galleryHideViewer},
        {"galleryHideInfo", galleryHideInfo},
        {"visionLibBoxes", visionLibBoxes},
        {"lastTab", activeTab},
        {"visionLibFolders", visionLibFolders},
        {"visionLibBoxSize", visionLibBoxSize},
        {"visionLibImageFrac", visionLibImageFrac},
        {"visionLibSideBySide", visionLibSideBySide},
        {"showNsfwBadges", showNsfwBadges},
        {"blurNsfw", blurNsfw},
        {"showHelpers", showHelpers},
        {"charView", charView},
        {"charTile", charTile},
        {"scanSubfolders", scanSubfolders},
        {"watchFolder", watchFolder},
        {"hideVisionFolder", hideVisionFolder},
        {"hideDuplicateCopies", hideDuplicateCopies},
        {"followNewest", followNewest},
        {"showGroupHeaders", showGroupHeaders},
        {"pixelFilter", filters.pixel},
        {"videoVolume", videoVolume},
        {"videoMuted", videoMuted},
        {"videoLoop", videoLoop},
        {"videoAutoplay", videoAutoplay},
        {"lightTheme", lightTheme},
        {"accent", accent},
        {"uiScale", uiScaleMul},
        {"slideshowSeconds", slideshowSeconds},
        {"searchHistory", searchHistory},
        {"recentFolders", recentFolders},
        {"window", {{"x", winX}, {"y", winY}, {"w", winW}, {"h", winH}, {"maximized", winMaximized}}},
    };
    nlohmann::json saved = nlohmann::json::array();
    for (const auto& [name, query] : savedSearches) saved.push_back({{"name", name}, {"query", query}});
    j["savedSearches"] = std::move(saved);
    std::error_code ec;
    fs::create_directories(AppDataDir(), ec);
    std::ofstream out(UiSettingsPath(), std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2);
}

// ---------------------------------------------------------------------------
// Ratings
// ---------------------------------------------------------------------------

namespace {
std::wstring RatingsPath() { return AppDataDir() + L"\\ratings.json"; }
std::wstring PHashPath() { return AppDataDir() + L"\\cache\\phash_cache.json"; }
std::wstring Utf8ToWideLocal(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}
} // namespace

void AppState::LoadRatings() {
    std::ifstream f(RatingsPath(), std::ios::binary);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    std::lock_guard<std::mutex> lock(ratingsMutex_);
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_number_integer()) continue;
        int stars = std::clamp(it.value().get<int>(), 0, 5);
        if (stars > 0) ratings_[ToLowerCopy(Utf8ToWideLocal(it.key()))] = stars;
    }
}

void AppState::SaveRatings() {
    nlohmann::json j = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lock(ratingsMutex_);
        for (const auto& [path, stars] : ratings_) j[NarrowUtf8(path)] = stars;
        ratingsDirty_ = false;
    }
    std::error_code ec;
    fs::create_directories(AppDataDir(), ec);
    std::wstring tmp = RatingsPath() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    fs::rename(tmp, RatingsPath(), ec);
    if (ec) fs::remove(tmp, ec);
}

void AppState::SetRating(const std::vector<int>& indices, int rating) {
    rating = std::clamp(rating, 0, 5);
    {
        std::lock_guard<std::mutex> lock(ratingsMutex_);
        for (int idx : indices) {
            if (idx < 0 || idx >= static_cast<int>(entries.size())) continue;
            entries[idx].rating = rating;
            std::wstring key = ToLowerCopy(entries[idx].path);
            if (rating > 0) ratings_[key] = rating;
            else ratings_.erase(key);
        }
        ratingsDirty_ = true;
        ratingsChangedTick_ = GetTickCount64();
    }
    filterEpoch++; // sorting/filtering by rating must refresh
}

void AppState::SaveRatingsIfDirty(bool force) {
    bool dirty;
    {
        std::lock_guard<std::mutex> lock(ratingsMutex_);
        dirty = ratingsDirty_ && (force || GetTickCount64() - ratingsChangedTick_ > 1500);
    }
    if (dirty) SaveRatings();
}

// ---------------------------------------------------------------------------
// Search history, restriction, compare
// ---------------------------------------------------------------------------

void AppState::PushSearchHistory(const std::string& query) {
    if (query.empty()) return;
    searchHistory.erase(std::remove(searchHistory.begin(), searchHistory.end(), query), searchHistory.end());
    searchHistory.insert(searchHistory.begin(), query);
    if (searchHistory.size() > 15) searchHistory.resize(15);
}

void AppState::SetRestrict(const std::vector<int>& indices) {
    restrictMask.assign(entries.size(), 0);
    for (int i : indices) {
        if (i >= 0 && i < static_cast<int>(entries.size())) restrictMask[i] = 1;
    }
    restrictActive = true;
    restrictVersion++;
    filterEpoch++;
}

void AppState::ClearRestrict() {
    if (!restrictActive) return;
    restrictActive = false;
    restrictMask.clear();
    restrictLabel.clear();
    restrictVersion++;
    filterEpoch++;
}

void AppState::OpenCompare(int a, int b) {
    if (a < 0 || b < 0 || a >= static_cast<int>(entries.size()) || b >= static_cast<int>(entries.size())) return;
    CloseCompare();
    compareA = a;
    compareB = b;
    std::string err;
    LoadImageTexture(device_, entries[a].path, compareTex[0], &err);
    LoadImageTexture(device_, entries[b].path, compareTex[1], &err);
    showCompare = true;
}

void AppState::CloseCompare() {
    ReleaseImage(compareTex[0]);
    ReleaseImage(compareTex[1]);
    compareA = compareB = -1;
    showCompare = false;
}

// ---------------------------------------------------------------------------
// Near-duplicate scan
// ---------------------------------------------------------------------------

void AppState::LoadPHashCache() {
    std::ifstream f(PHashPath(), std::ios::binary);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_array()) return;
    std::lock_guard<std::mutex> lock(phashMutex_);
    for (auto& e : j) {
        if (!e.is_array() || e.size() != 4 || !e[0].is_string()) continue;
        PHashRec r;
        r.mtime = e[1].get<int64_t>();
        r.size = e[2].get<uintmax_t>();
        r.hash = std::stoull(e[3].get<std::string>(), nullptr, 16);
        phashCache_[ToLowerCopy(Utf8ToWideLocal(e[0].get<std::string>()))] = r;
    }
}

void AppState::SavePHashCache() {
    nlohmann::json j = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(phashMutex_);
        char hex[20];
        for (const auto& [path, r] : phashCache_) {
            snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(r.hash));
            j.push_back({NarrowUtf8(path), r.mtime, r.size, std::string(hex)});
        }
    }
    std::error_code ec;
    fs::create_directories(fs::path(PHashPath()).parent_path(), ec);
    std::wstring tmp = PHashPath() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    fs::rename(tmp, PHashPath(), ec);
    if (ec) fs::remove(tmp, ec);
}

void AppState::CancelSimilarScan() {
    if (similarThread_.joinable()) {
        similarCancel_ = true;
        similarThread_.join();
    }
    similarCancel_ = false;
    similarRunning_ = false;
}

void AppState::StartSimilarScan(int maxDistance) {
    CancelSimilarScan();
    if (entries.empty()) return;
    struct Item {
        int index;
        std::wstring path, key;
        int64_t mtime;
        uintmax_t size;
    };
    std::vector<Item> items;
    items.reserve(entries.size());
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        const ImageEntry& e = entries[i];
        items.push_back({i, e.path, ToLowerCopy(e.path), MtimeTicks(e.mtime), e.size});
    }
    similarFolder_ = currentFolder;
    similarCurrent_ = 0;
    similarTotal_ = static_cast<int>(items.size());
    similarRunning_ = true;
    {
        std::lock_guard<std::mutex> lock(similarMutex_);
        similarDone_ = false;
    }
    similarThread_ = std::thread([this, items = std::move(items), maxDistance]() {
        std::vector<HashedItem> hashed;
        hashed.reserve(items.size());
        int failed = 0;
        try {
            if (diskCacheReady_.valid()) diskCacheReady_.wait();
            for (const Item& item : items) {
                if (similarCancel_.load()) break;
                uint64_t hash = 0;
                bool have = false;
                {
                    std::lock_guard<std::mutex> lock(phashMutex_);
                    auto it = phashCache_.find(item.key);
                    if (it != phashCache_.end() && it->second.mtime == item.mtime && it->second.size == item.size) {
                        hash = it->second.hash;
                        have = true;
                    }
                }
                if (!have) {
                    // Prefer the small cached thumbnail; fall back to decoding the file.
                    std::vector<unsigned char> pixels;
                    int w = 0, h = 0;
                    DiskCache::Record rec;
                    std::vector<unsigned char> thumbBytes;
                    if (diskCache_.TryGet(item.key, rec) && diskCache_.ReadThumbBytes(rec, thumbBytes)) {
                        int comp = 0;
                        unsigned char* px = stbi_load_from_memory(thumbBytes.data(), static_cast<int>(thumbBytes.size()), &w, &h, &comp, 4);
                        if (px) {
                            hash = DHashFromRGBA(px, w, h);
                            stbi_image_free(px);
                            have = true;
                        }
                    }
                    if (!have && DecodeImageFileRGBA(item.path, pixels, w, h)) {
                        hash = DHashFromRGBA(pixels.data(), w, h);
                        have = true;
                    }
                    if (have) {
                        std::lock_guard<std::mutex> lock(phashMutex_);
                        phashCache_[item.key] = {item.mtime, item.size, hash};
                    }
                }
                if (have) hashed.push_back({item.index, hash});
                else ++failed;
                similarCurrent_++;
            }
            if (!similarCancel_.load()) SavePHashCache();
        } catch (...) {
        }

        std::vector<std::vector<int>> groups;
        std::string message;
        if (similarCancel_.load()) {
            message = "Cancelled.";
        } else {
            groups = GroupSimilar(hashed, maxDistance, &similarCancel_);
            size_t images = 0;
            for (auto& g : groups) images += g.size();
            message = groups.empty() ? "No similar images found."
                                     : std::to_string(groups.size()) + " group(s), " + std::to_string(images) + " images.";
            if (failed) message += " " + std::to_string(failed) + " could not be read.";
        }
        std::lock_guard<std::mutex> lock(similarMutex_);
        similarPending_ = std::move(groups);
        similarPendingMessage_ = std::move(message);
        similarDone_ = true;
        similarRunning_ = false;
    });
}

void AppState::PumpSimilarResults() {
    std::lock_guard<std::mutex> lock(similarMutex_);
    if (!similarDone_) return;
    similarDone_ = false;
    if (similarFolder_ == currentFolder) {
        similarGroups = std::move(similarPending_);
        similarMessage = std::move(similarPendingMessage_);
    }
    similarPending_.clear();
}

// ---------------------------------------------------------------------------
// Image-based people counting
// ---------------------------------------------------------------------------

namespace {
std::wstring PeopleCachePath() { return AppDataDir() + L"\\cache\\people_cache.json"; }
} // namespace

void AppState::LoadPeopleCache() {
    peopleModelId_ = PersonDetector::ModelId();
    std::ifstream f(PeopleCachePath(), std::ios::binary);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    // Counts from a different model aren't comparable; start over.
    if (!j.is_object() || j.value("model", std::string()) != peopleModelId_) return;
    if (!j.contains("entries") || !j["entries"].is_array()) return;
    std::lock_guard<std::mutex> lock(peopleCacheMutex_);
    for (auto& e : j["entries"]) {
        if (!e.is_array() || e.size() != 4 || !e[0].is_string()) continue;
        PeopleRec r;
        r.mtime = e[1].get<int64_t>();
        r.size = e[2].get<uintmax_t>();
        r.count = e[3].get<int>();
        peopleCache_[ToLowerCopy(Utf8ToWideLocal(e[0].get<std::string>()))] = r;
    }
}

void AppState::SavePeopleCache() {
    nlohmann::json entries = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(peopleCacheMutex_);
        for (const auto& [path, r] : peopleCache_) entries.push_back({NarrowUtf8(path), r.mtime, r.size, r.count});
    }
    nlohmann::json j = {{"model", peopleModelId_}, {"entries", std::move(entries)}};
    std::error_code ec;
    fs::create_directories(fs::path(PeopleCachePath()).parent_path(), ec);
    std::wstring tmp = PeopleCachePath() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    fs::rename(tmp, PeopleCachePath(), ec);
    if (ec) fs::remove(tmp, ec);
}

void AppState::CancelPeopleScan() {
    if (peopleThread_.joinable()) {
        peopleCancel_ = true;
        peopleThread_.join();
    }
    peopleCancel_ = false;
    peopleRunning_ = false;
}

int AppState::DetectedPeopleCount() const {
    int n = 0;
    for (const ImageEntry& e : entries) n += e.peopleFromImage ? 1 : 0;
    return n;
}

void AppState::StartPeopleScan(const std::vector<int>& indices) {
    CancelPeopleScan();
    struct Item {
        size_t index;
        std::wstring path, key;
        int64_t mtime;
        uintmax_t size;
    };
    std::vector<Item> items;
    for (int idx : indices) {
        if (idx < 0 || idx >= static_cast<int>(entries.size())) continue;
        const ImageEntry& e = entries[idx];
        if (e.peopleFromImage) continue; // already counted
        items.push_back({static_cast<size_t>(idx), e.path, ToLowerCopy(e.path), MtimeTicks(e.mtime), e.size});
    }
    peopleMessage.clear();
    if (items.empty()) {
        peopleMessage = "Everything here has already been counted.";
        return;
    }
    peopleCurrent_ = 0;
    peopleTotal_ = static_cast<int>(items.size());
    peopleRunning_ = true;
    peopleThread_ = std::thread([this, items = std::move(items)]() {
        std::string message, device;
        int failed = 0, done = 0;
        try {
            std::string err;
            if (!personDetector_.EnsureLoaded(&err)) {
                message = "People detection unavailable: " + err;
            } else {
                device = personDetector_.DeviceName();
                std::atomic<size_t> next{0};
                auto worker = [&]() {
                    for (size_t i = next++; i < items.size() && !peopleCancel_.load(); i = next++) {
                        const Item& it = items[i];
                        int count = -1;
                        try {
                            count = personDetector_.CountPeopleFile(it.path, nullptr);
                        } catch (...) {
                        }
                        if (count >= 0) {
                            {
                                std::lock_guard<std::mutex> lock(peopleCacheMutex_);
                                peopleCache_[it.key] = {it.mtime, it.size, count};
                            }
                            std::lock_guard<std::mutex> lock(peopleResultsMutex_);
                            peoplePending_.push_back({it.index, it.path, count});
                        } else {
                            ++failed;
                        }
                        peopleCurrent_++;
                    }
                };
                // Decoding large images dominates; decode on several threads, inference is serialized.
                unsigned threadCount = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
                std::vector<std::thread> pool;
                for (unsigned t = 1; t < threadCount; ++t) pool.emplace_back(worker);
                worker();
                for (auto& t : pool) t.join();
                done = peopleCurrent_.load();
                message = std::string(peopleCancel_ ? "Cancelled. " : "") + "Counted people in " + std::to_string(done - failed) +
                          " image(s)." + (failed ? " " + std::to_string(failed) + " could not be read." : std::string());
                SavePeopleCache();
            }
        } catch (...) {
            message = "Unexpected error while detecting people.";
        }
        std::lock_guard<std::mutex> lock(peopleResultsMutex_);
        peopleFinishedMessage_ = std::move(message);
        peopleFinishedDevice_ = std::move(device);
        peopleFinished_ = true;
        peopleRunning_ = false;
    });
}

void AppState::PumpPeopleResults() {
    std::vector<PeopleResult> results;
    bool finished = false;
    std::string message, device;
    {
        std::lock_guard<std::mutex> lock(peopleResultsMutex_);
        results.swap(peoplePending_);
        if (peopleFinished_) {
            finished = true;
            message = std::move(peopleFinishedMessage_);
            device = std::move(peopleFinishedDevice_);
            peopleFinished_ = false;
        }
    }
    if (!results.empty()) {
        for (const PeopleResult& r : results) {
            if (r.index < entries.size() && entries[r.index].path == r.path) {
                entries[r.index].people = r.count;
                entries[r.index].peopleFromImage = true;
            }
        }
        filterEpoch++;
    }
    if (finished) {
        peopleMessage = message;
        if (!device.empty()) peopleDevice = device;
        SetStatus(message);
    }
}

void AppState::ForgetDetectedPeople() {
    CancelPeopleScan();
    {
        std::lock_guard<std::mutex> lock(peopleCacheMutex_);
        peopleCache_.clear();
    }
    SavePeopleCache();
    for (ImageEntry& e : entries) {
        e.peopleFromImage = false;
        e.people = EstimatePeopleCount(e.metadata.parsed.positivePrompt);
    }
    filterEpoch++;
    peopleMessage = "Back to prompt-based estimates.";
}

// ---------------------------------------------------------------------------
// Vision output modes
// ---------------------------------------------------------------------------

std::string AppState::StyleDirective() const {
    if (visionStyle.empty()) return "";
    if (visionStyle == "custom") return visionStyleCustom;
    for (int i = 0; i < VisionStyleCount(); ++i) {
        if (visionStyle == VisionStyleAt(i).key) return VisionStyleAt(i).directive;
    }
    return "";
}

std::string AppState::StyleSlugKey() const {
    if (StyleDirective().empty()) return "";
    return visionStyle == "custom" ? "custom-" + StyleSlug(visionStyleCustom) : visionStyle;
}

int AppState::CountVisionTodo(const std::vector<std::wstring>& paths) const {
    const VisionPlan plan = MakeVisionPlan();
    const bool skip = visionSkipDone && paths.size() > 1 && !plan.saveDir.empty();
    int n = 0;
    for (const std::wstring& path : paths) {
        if (skip && HasSavedResult(path, SaveDirFor(path, plan.saveDir), plan.options.txtSuffix)) continue;
        ++n;
    }
    return n;
}

void AppState::RequestVisionBatch(const std::vector<int>& indices) {
    std::vector<std::wstring> paths;
    for (int idx : indices) {
        if (idx >= 0 && idx < static_cast<int>(entries.size()) && !entries[idx].missing) paths.push_back(entries[idx].path);
    }
    RequestVisionPaths(paths);
}

void AppState::RequestVisionPaths(const std::vector<std::wstring>& paths) {
    if (visionWarnBig && static_cast<int>(paths.size()) > visionWarnOver && CountVisionTodo(paths) > visionWarnOver) {
        visionPending = paths;
        showVisionConfirm = true;
        return;
    }
    DescribePaths(paths);
}

std::string AppState::VisionModeKey() const {
    switch (visionMode) {
        case VisionMode::Tags: return "tags";
        case VisionMode::VideoScene:
            return "video-" + std::to_string(videoOptions.seconds) + "s-" + (videoOptions.timeline ? "t" : "p");
        default: return StyleDirective().empty() ? "" : "style-" + StyleSlugKey();
    }
}

std::wstring AppState::VisionTxtSuffix() const {
    switch (visionMode) {
        case VisionMode::Tags: return L".tags";
        case VisionMode::VideoScene: return L".video-" + std::to_wstring(videoOptions.seconds) + L"s";
        default: return StyleDirective().empty() ? L"" : L".style-" + WidenUtf8(StyleSlugKey());
    }
}

std::wstring AppState::VisionTextKey(const std::wstring& path, const std::string& modeKey) {
    if (modeKey.empty()) return path;
    return path + L"|" + Utf8ToWideLocal(modeKey);
}

// ---------------------------------------------------------------------------
// Live folder watching
// ---------------------------------------------------------------------------

void AppState::RebuildPathIndex() {
    pathIndex_.clear();
    pathIndex_.reserve(entries.size());
    copyIndex_.clear();
    for (size_t i = 0; i < entries.size(); ++i) {
        pathIndex_[ToLowerCopy(entries[i].path)] = static_cast<int>(i);
        if (hideDuplicateCopies) copyIndex_[CopyKey(entries[i].path, entries[i].size, entries[i].mtime)] = static_cast<int>(i);
    }
}

void AppState::StopLiveWorker() {
    {
        std::lock_guard<std::mutex> lock(liveMutex_);
        liveStop_ = true;
        liveQueue_.clear();
    }
    liveCv_.notify_all();
    if (liveThread_.joinable()) liveThread_.join();
}

void AppState::EnqueueLive(LiveJob job) {
    {
        std::lock_guard<std::mutex> lock(liveMutex_);
        if (liveStop_) liveStop_ = false; // restart after a stop (never happens before the destructor in practice)
        liveQueue_.push_back(std::move(job));
    }
    if (!liveThread_.joinable()) {
        liveThread_ = std::thread([this]() {
            if (diskCacheReady_.valid()) diskCacheReady_.wait();
            for (;;) {
                LiveJob job;
                {
                    std::unique_lock<std::mutex> lock(liveMutex_);
                    liveCv_.wait(lock, [this]() { return liveStop_ || !liveQueue_.empty(); });
                    if (liveStop_) return;
                    job = std::move(liveQueue_.front());
                    liveQueue_.pop_front();
                }
                ProcessFile(job.index, job.path, job.mtime, job.size, /*persist=*/false);
            }
        });
    }
    liveCv_.notify_one();
}

void AppState::PumpFolderWatch() {
    if (listing_.load()) return; // entries aren't installed yet
    // Follow the toggle and folder/recursion changes.
    if (!watchFolder || currentFolder.empty()) {
        if (watcher_.IsRunning() || !watchedFolder_.empty()) {
            watcher_.Stop();
            watchedFolder_.clear();
        }
        watchPending_.clear();
        return;
    }
    if (watchedFolder_ != currentFolder || watchedRecursive_ != scanSubfolders) {
        watcher_.Start(currentFolder, scanSubfolders);
        watchedFolder_ = currentFolder;
        watchedRecursive_ = scanSubfolders;
    }

    const unsigned long long now = GetTickCount64();
    bool changed = false, overflow = false;
    for (const FolderWatcher::Change& ch : watcher_.Drain()) {
        if (ch.kind == FolderWatcher::Kind::Overflow) {
            overflow = true;
            continue;
        }
        const std::wstring key = ToLowerCopy(ch.path);
        if (ch.kind == FolderWatcher::Kind::Removed) {
            watchPending_.erase(key);
            auto it = pathIndex_.find(key);
            if (it != pathIndex_.end() && it->second < static_cast<int>(entries.size()) && !entries[it->second].missing) {
                entries[it->second].missing = true;
                missingCount_++;
                changed = true;
            }
        } else if (IsSupportedImage(fs::path(ch.path))) {
            if (hideVisionFolder && !visionOutputDir.empty()) {
                const std::wstring ex = NormalizedDir(WidenUtf8(visionOutputDir));
                const std::wstring cur = NormalizedDir(currentFolder);
                if (!ex.empty() && cur.compare(0, ex.size(), ex) != 0 && NormalizedDir(ch.path).compare(0, ex.size(), ex) == 0) continue;
            }
            watchPending_[key] = {ch.path, now};
        }
    }
    if (overflow) {
        ReloadFolder(); // events were lost: the cheap, correct answer is a rescan
        return;
    }

    // A file is picked up once it has been quiet for a moment, so images that are
    // still being written aren't read half-finished.
    int added = 0;
    std::string lastAddedName;
    for (auto it = watchPending_.begin(); it != watchPending_.end();) {
        if (now - it->second.tick < 1200) {
            ++it;
            continue;
        }
        const std::wstring key = it->first;
        const std::wstring path = it->second.path;
        it = watchPending_.erase(it);

        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) continue;
        uintmax_t size = fs::file_size(path, ec);
        if (ec || size == 0) continue;
        fs::file_time_type mtime = fs::last_write_time(path, ec);
        if (ec) continue;

        auto existing = pathIndex_.find(key);
        if (existing == pathIndex_.end() && hideDuplicateCopies) {
            auto dup = copyIndex_.find(CopyKey(path, size, mtime));
            if (dup != copyIndex_.end()) { // an extra copy of an image that is already shown
                ++hiddenCopies_;
                continue;
            }
        }
        if (existing == pathIndex_.end()) {
            ImageEntry entry;
            entry.path = path;
            std::error_code relEc;
            fs::path rel = fs::relative(path, currentFolder, relEc);
            entry.displayName = (relEc || rel.empty()) ? fs::path(path).filename().wstring() : rel.wstring();
            entry.size = size;
            entry.mtime = mtime;
            FileTimeYearMonth(mtime, entry.year, entry.month);
            entry.arrivedTick = GetTickCount64();
            entries.push_back(std::move(entry));
            entriesSortedByName_ = false; // appended at the end
            const int index = static_cast<int>(entries.size()) - 1;
            pathIndex_[key] = index;
            if (hideDuplicateCopies) copyIndex_[CopyKey(path, size, mtime)] = index;
            EnqueueLive({static_cast<size_t>(index), path, mtime, size});
            if (followNewest) liveFollowIndex_ = index;
            ++added;
            lastAddedName = NarrowUtf8(fs::path(path).filename().wstring());
            changed = true;
        } else {
            ImageEntry& e = entries[existing->second];
            if (e.missing) {
                e.missing = false;
                missingCount_--;
                changed = true;
            }
            if (e.mtime != mtime || e.size != size) {
                e.mtime = mtime;
                e.size = size;
                FileTimeYearMonth(mtime, e.year, e.month);
                EnqueueLive({static_cast<size_t>(existing->second), path, mtime, size}); // re-read the changed file
                changed = true;
            }
        }
    }
    if (added == 1) SetStatus("New image: " + lastAddedName);
    else if (added > 1) SetStatus(std::to_string(added) + " new images");
    if (changed) filterEpoch++;
    totalCount_ = static_cast<int>(entries.size());
}

// ---------------------------------------------------------------------------
// Moving per-file data along with a renamed file
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Picture quality (aesthetic) scores
// ---------------------------------------------------------------------------
namespace {
std::wstring QualityCachePath() { return AppDataDir() + L"\\cache\\quality_scores.json"; }
} // namespace

void AppState::LoadQualityCache() {
    qualityModelId_ = AestheticScorer::ModelId();
    std::ifstream f(QualityCachePath(), std::ios::binary);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    // scores from other models aren't comparable
    if (!j.is_object() || j.value("model", std::string()) != qualityModelId_ || !j.contains("entries") || !j["entries"].is_array()) return;
    std::lock_guard<std::mutex> lock(qualityMutex_);
    for (auto& e : j["entries"]) {
        if (!e.is_object() || !e.contains("path") || !e["path"].is_string()) continue;
        CachedQuality c;
        c.mtime = e.value("mtime", int64_t{0});
        c.size = e.value("size", uintmax_t{0});
        c.anime = e.value("anime", -1.0f);
        c.general = e.value("general", -1.0f);
        c.prob = e.value("prob", -1.0f);
        c.kind = e.value("kind", -1);
        c.kindConf = e.value("kc", -1.0f);
        qualityCache_[Utf8ToWideLocal(e["path"].get<std::string>())] = c;
    }
}

void AppState::SaveQualityCache() {
    nlohmann::json arr = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(qualityMutex_);
        for (const auto& [path, c] : qualityCache_) {
            arr.push_back({{"path", NarrowUtf8(path)}, {"mtime", c.mtime}, {"size", c.size}, {"anime", c.anime}, {"general", c.general}, {"prob", c.prob}, {"kind", c.kind}, {"kc", c.kindConf}});
        }
    }
    nlohmann::json j;
    j["model"] = qualityModelId_;
    j["entries"] = std::move(arr);
    const std::wstring path = QualityCachePath();
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << j.dump();
    }
    fs::rename(tmp, path, ec);
}

bool AppState::LookupQualityCache(ImageEntry& e) {
    std::lock_guard<std::mutex> lock(qualityMutex_);
    auto it = qualityCache_.find(ToLowerCopy(e.path));
    if (it == qualityCache_.end() || it->second.mtime != MtimeTicks(e.mtime) || it->second.size != e.size) return false;
    e.qualityAnime = it->second.anime;
    e.qualityGeneral = it->second.general;
    e.qualityAnimeProb = it->second.prob;
    e.styleKind = it->second.kind;
    e.styleConf = it->second.kindConf;
    return true;
}

void AppState::CancelQualityJob() {
    if (qualityThread_.joinable()) {
        qualityCancel_ = true;
        qualityThread_.join();
    }
    qualityCancel_ = false;
    qualityRunning_ = false;
}

void AppState::StartQualityJob(const std::vector<int>& indices) {
    if (qualityRunning_.load()) return;
    if (!AestheticScorer::ModelsPresent()) {
        SetStatus("The quality models are missing: put aesthetic_clip.onnx, aesthetic_anime.onnx and aesthetic.json in the models folder.");
        return;
    }
    struct Item {
        size_t index;
        std::wstring path;
        fs::file_time_type mtime;
        uintmax_t size;
    };
    std::vector<Item> items;
    for (int idx : indices) {
        if (idx < 0 || idx >= static_cast<int>(entries.size())) continue;
        const ImageEntry& e = entries[idx];
        if (e.missing || e.Quality() >= 0.0f) continue; // scored already (or cached)
        items.push_back({static_cast<size_t>(idx), e.path, e.mtime, e.size});
    }
    if (items.empty()) {
        SetStatus("Everything shown is already scored.");
        return;
    }
    CancelQualityJob();
    qualityCurrent_ = 0;
    qualityTotal_ = static_cast<int>(items.size());
    qualityRunning_ = true;
    qualityThread_ = std::thread([this, items = std::move(items)]() {
        std::string message;
        try {
            std::string err;
            if (!aesthetic_.EnsureLoaded(&err)) {
                message = err;
            } else {
                std::atomic<size_t> next{0};
                std::atomic<int> failed{0};
                auto worker = [&]() {
                    for (size_t i = next++; i < items.size() && !qualityCancel_.load(); i = next++) {
                        const Item& it = items[i];
                        AestheticScorer::Scores sc;
                        bool ok = false;
                        try {
                            ok = aesthetic_.ScoreFile(it.path, sc, nullptr);
                        } catch (...) {
                        }
                        if (ok) {
                            std::lock_guard<std::mutex> lock(qualityMutex_);
                            qualityCache_[ToLowerCopy(it.path)] = {MtimeTicks(it.mtime), it.size, sc.anime, sc.general, sc.animeProb, sc.style, sc.styleConf};
                        } else {
                            ++failed;
                        }
                        {
                            std::lock_guard<std::mutex> lock(qualityResultsMutex_);
                            if (ok) qualityPending_.push_back({it.index, it.path, sc.anime, sc.general, sc.animeProb, sc.style, sc.styleConf});
                        }
                        const int done = ++qualityCurrent_;
                        if (done % 1500 == 0) SaveQualityCache(); // keep what is finished if the app is closed halfway
                    }
                };
                // decoding big pictures dominates: several threads decode, inference is serialized in the scorer
                const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
                std::vector<std::thread> pool;
                for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
                worker();
                for (auto& t : pool) t.join();
                message = std::string(qualityCancel_ ? "Cancelled. " : "") + "Scored " + std::to_string(qualityCurrent_.load() - failed.load()) +
                          " picture(s) on " + aesthetic_.DeviceName() + "." + (failed ? " " + std::to_string(failed.load()) + " could not be read." : std::string());
            }
        } catch (...) {
            message = "Unexpected error while scoring.";
        }
        try {
            SaveQualityCache();
        } catch (...) {
        }
        std::lock_guard<std::mutex> lock(qualityResultsMutex_);
        qualityMessage_ = std::move(message);
        qualityFinished_ = true;
        qualityRunning_ = false;
    });
}

void AppState::PumpQualityResults() {
    std::vector<QualityResult> results;
    std::string message;
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(qualityResultsMutex_);
        results.swap(qualityPending_);
        if (qualityFinished_) {
            finished = true;
            message = std::move(qualityMessage_);
            qualityFinished_ = false;
        }
    }
    if (!results.empty()) {
        for (const QualityResult& r : results) {
            if (r.index < entries.size() && entries[r.index].path == r.path && !entries[r.index].clefGraded) {
                entries[r.index].qualityAnime = r.anime;
                entries[r.index].qualityGeneral = r.general;
                entries[r.index].qualityAnimeProb = r.prob;
                entries[r.index].styleKind = r.kind;
                entries[r.index].styleConf = r.kindConf;
            }
        }
        filterEpoch++;
    }
    if (finished) {
        SetStatus(message);
        if (qualityThread_.joinable()) qualityThread_.join();
    }
}


AppState::MoveReport AppState::MoveImages(const std::vector<std::wstring>& imagePaths, const std::wstring& destDir) {
    MoveReport report;
    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (!fs::is_directory(destDir, ec)) {
        report.message = "Could not use that folder.";
        report.failed = static_cast<int>(imagePaths.size());
        return report;
    }
    auto lowerEq = [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; };
    auto textKind = [](std::wstring suffix) { // what follows the image's stem in a result file name ("" for <name>.txt)
        std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::towlower);
        if (suffix.empty() || suffix == L".tags") return true;
        if (suffix.rfind(L".style-", 0) == 0 && suffix.size() > 7) return true;
        if (suffix.rfind(L".video-", 0) == 0 && suffix.size() > 8 && suffix.back() == L's') return true;
        return false;
    };
    for (const std::wstring& src : imagePaths) {
        fs::path from(src);
        if (!fs::exists(from, ec)) { ++report.failed; continue; }
        if (lowerEq(from.parent_path().wstring(), fs::path(destDir).wstring())) continue; // already there
        // free file name in the destination
        fs::path to = fs::path(destDir) / from.filename();
        for (int n = 2; fs::exists(to, ec); ++n) {
            to = fs::path(destDir) / (from.stem().wstring() + L" (" + std::to_wstring(n) + L")" + from.extension().wstring());
        }
        if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) { ++report.failed; continue; }
        ++report.moved;
        MovePathMetadata(from.wstring(), to.wstring());

        // the result texts that belong to it
        const std::wstring oldStem = from.stem().wstring(), newStem = to.stem().wstring();
        WIN32_FIND_DATAW fd;
        const std::wstring pattern = from.parent_path().wstring() + L"\\" + oldStem + L".*";
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            std::vector<std::wstring> texts;
            do {
                const std::wstring name = fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (name.size() < oldStem.size() + 4 || _wcsicmp(name.c_str() + name.size() - 4, L".txt") != 0) continue;
                if (_wcsnicmp(name.c_str(), oldStem.c_str(), oldStem.size()) != 0) continue;
                const std::wstring middle = name.substr(oldStem.size(), name.size() - oldStem.size() - 4); // between the stem and ".txt"
                if (textKind(middle)) texts.push_back(name);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            for (const std::wstring& name : texts) {
                const std::wstring tail = name.substr(oldStem.size());
                const fs::path tfrom = from.parent_path() / name, tto = fs::path(destDir) / (newStem + tail);
                if (!fs::exists(tto, ec) && MoveFileExW(tfrom.c_str(), tto.c_str(), MOVEFILE_COPY_ALLOWED)) ++report.texts;
            }
        }

        // the gallery's own entry follows the file (or is dropped when it left the opened folder)
        auto found = pathIndex_.find(ToLowerCopy(from.wstring()));
        if (found != pathIndex_.end() && found->second < static_cast<int>(entries.size())) {
            const int idx = found->second;
            ImageEntry& e = entries[idx];
            const std::wstring root = currentFolder;
            const std::wstring np = to.wstring();
            const bool inside = !root.empty() && np.size() > root.size() + 1 && _wcsnicmp(np.c_str(), root.c_str(), root.size()) == 0 &&
                                (np[root.size()] == L'\\' || np[root.size()] == L'/');
            pathIndex_.erase(found);
            if (inside) {
                e.path = np;
                e.displayName = np.substr(root.size() + 1);
                pathIndex_[ToLowerCopy(np)] = idx;
            } else {
                e.missing = true;
            }
        }
    }
    folderGeneration++;
    filterEpoch++;
    MarkDescribedDirty();
    RefreshVisionLibrary();
    report.message = "Moved " + std::to_string(report.moved) + " image" + (report.moved == 1 ? "" : "s") +
                     (report.texts ? " and " + std::to_string(report.texts) + " result text" + (report.texts == 1 ? "" : "s") : std::string()) +
                     (report.failed ? ", " + std::to_string(report.failed) + " could not be moved" : std::string()) + ".";
    return report;
}

bool AppState::CreateFolder(const std::wstring& parent, const std::string& name, std::string* error) {
    std::string clean;
    for (unsigned char c : name) clean += (c < 32 || std::strchr("<>:\"/\\|?*", c)) ? '_' : static_cast<char>(c);
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
    while (!clean.empty() && clean.front() == ' ') clean.erase(clean.begin());
    if (clean.empty()) {
        if (error) *error = "Type a name for the folder.";
        return false;
    }
    std::error_code ec;
    const fs::path dir = fs::path(parent) / Utf8ToWideLocal(clean);
    if (fs::exists(dir, ec)) {
        if (error) *error = "A folder with that name is already there.";
        return false;
    }
    if (!fs::create_directories(dir, ec) || ec) {
        if (error) *error = "Could not create the folder.";
        return false;
    }
    folderGeneration++;
    RefreshVisionLibrary();
    return true;
}

void AppState::MovePathMetadata(const std::wstring& from, const std::wstring& to) {
    const std::wstring a = ToLowerCopy(from), b = ToLowerCopy(to);
    auto move = [&](auto& map) {
        auto it = map.find(a);
        if (it == map.end()) return;
        auto value = std::move(it->second);
        map.erase(it);
        map[b] = std::move(value);
    };
    {
        std::lock_guard<std::mutex> lock(ratingsMutex_);
        auto it = ratings_.find(a);
        if (it != ratings_.end()) {
            int stars = it->second;
            ratings_.erase(it);
            ratings_[b] = stars;
            ratingsDirty_ = true;
            ratingsChangedTick_ = GetTickCount64();
        }
    }
    { std::lock_guard<std::mutex> lock(nsfwCacheMutex_); move(nsfwCache_); }
    { std::lock_guard<std::mutex> lock(peopleCacheMutex_); move(peopleCache_); }
    { std::lock_guard<std::mutex> lock(phashMutex_); move(phashCache_); }
    { std::lock_guard<std::mutex> lock(cacheMutex_); move(thumbCache_); }
    diskCache_.Rekey(a, b);
}

// ---------------------------------------------------------------------------
// Clean (metadata-free) copies
// ---------------------------------------------------------------------------

void AppState::CancelCleanExport() {
    if (cleanThread_.joinable()) {
        cleanCancel_ = true;
        cleanThread_.join();
    }
    cleanCancel_ = false;
    cleanRunning_ = false;
}

void AppState::StartCleanExport(const std::vector<int>& indices) {
    CancelCleanExport();
    struct Item {
        std::wstring path, name;
        std::string prompt; // text to keep for "prompt only"
        bool metadataKnown = false;
    };
    std::vector<Item> items;
    for (int idx : indices) {
        if (idx < 0 || idx >= static_cast<int>(entries.size())) continue;
        const ImageEntry& e = entries[idx];
        Item item;
        item.path = e.path;
        item.name = fs::path(e.path).filename().wstring();
        if (e.thumbReady) {
            item.prompt = e.metadata.parsed.positivePrompt;
            if (!e.metadata.parsed.negativePrompt.empty()) item.prompt += "\nNegative prompt: " + e.metadata.parsed.negativePrompt;
            item.metadataKnown = true;
        }
        items.push_back(std::move(item));
    }
    cleanMessage.clear();
    if (items.empty()) {
        cleanMessage = "Nothing selected.";
        return;
    }
    std::wstring dest = cleanExportDir.empty() ? (fs::path(currentFolder) / L"clean").wstring() : Utf8ToWideLocal(cleanExportDir);
    const StripMode mode = cleanMode == 1 ? StripMode::KeepPromptOnly : StripMode::RemoveAll;
    cleanCurrent_ = 0;
    cleanTotal_ = static_cast<int>(items.size());
    cleanRunning_ = true;
    cleanThread_ = std::thread([this, items = std::move(items), dest, mode]() {
        int ok = 0, failed = 0;
        std::string firstError;
        try {
            for (const Item& item : items) {
                if (cleanCancel_.load()) break;
                std::string prompt = item.prompt;
                if (mode == StripMode::KeepPromptOnly && !item.metadataKnown) {
                    ImageMetadata md = ReadImageMetadata(item.path);
                    prompt = md.parsed.positivePrompt;
                    if (!md.parsed.negativePrompt.empty()) prompt += "\nNegative prompt: " + md.parsed.negativePrompt;
                }
                // Never overwrite: "name (2).png" if a file of that name is already there.
                fs::path target = fs::path(dest) / item.name;
                std::error_code ec;
                for (int n = 2; fs::exists(target, ec); ++n) {
                    fs::path name(item.name);
                    target = fs::path(dest) / (name.stem().wstring() + L" (" + std::to_wstring(n) + L")" + name.extension().wstring());
                }
                std::string err;
                if (WriteCleanCopy(item.path, target.wstring(), mode, prompt, &err)) {
                    ++ok;
                } else {
                    ++failed;
                    if (firstError.empty()) firstError = NarrowUtf8(fs::path(item.path).filename().wstring()) + ": " + err;
                }
                cleanCurrent_++;
            }
        } catch (...) {
            firstError = "Unexpected error";
        }
        std::string message = std::string(cleanCancel_ ? "Cancelled. " : "") + "Wrote " + std::to_string(ok) + " clean cop" +
                              (ok == 1 ? "y" : "ies") + ".";
        if (failed) message += " " + std::to_string(failed) + " failed (" + firstError + ").";
        std::lock_guard<std::mutex> lock(cleanMutex_);
        cleanFinishedMessage_ = std::move(message);
        cleanFinishedDir_ = ok > 0 ? dest : std::wstring();
        cleanFinished_ = true;
        cleanRunning_ = false;
    });
}

void AppState::PumpCleanExport() {
    std::string message;
    std::wstring dir;
    {
        std::lock_guard<std::mutex> lock(cleanMutex_);
        if (!cleanFinished_) return;
        cleanFinished_ = false;
        message = std::move(cleanFinishedMessage_);
        dir = std::move(cleanFinishedDir_);
    }
    cleanMessage = message;
    SetStatus(message);
    if (cleanOpenWhenDone && !dir.empty()) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Batch rename (with undo)
// ---------------------------------------------------------------------------

namespace {

std::wstring RenameLogDir() { return AppDataDir() + L"\\logs"; }

// Newest rename log that hasn't been undone, or empty.
std::wstring FindLatestRenameLog() {
    std::error_code ec;
    std::wstring best;
    for (fs::directory_iterator it(RenameLogDir(), ec), end; !ec && it != end; it.increment(ec)) {
        std::wstring name = it->path().filename().wstring();
        if (name.rfind(L"rename_", 0) == 0 && it->path().extension() == L".json" && name.find(L".undone") == std::wstring::npos) {
            if (best.empty() || name > fs::path(best).filename().wstring()) best = it->path().wstring();
        }
    }
    return best;
}

} // namespace

void AppState::RefreshRenameLog() { latestRenameLog_ = FindLatestRenameLog(); }

void AppState::CancelRename() {
    if (renameThread_.joinable()) {
        renameCancel_ = true;
        renameThread_.join();
    }
    renameCancel_ = false;
    renameRunning_ = false;
}

void AppState::StartRename(const std::vector<std::pair<std::wstring, std::wstring>>& requested) {
    CancelRename();
    std::vector<std::pair<std::wstring, std::wstring>> moves;
    for (const auto& m : requested) {
        if (m.first != m.second) moves.push_back(m);
    }
    renameMessage.clear();
    if (moves.empty()) {
        renameMessage = "Nothing to rename.";
        return;
    }
    renameCurrent_ = 0;
    renameTotal_ = static_cast<int>(moves.size());
    renameRunning_ = true;
    renameThread_ = std::thread([this, moves = std::move(moves)]() {
        std::vector<std::pair<std::wstring, std::wstring>> done;
        int failed = 0;
        std::string firstError;
        try {
            if (diskCacheReady_.valid()) diskCacheReady_.wait();
            // Targets that are another file's current name (renaming a set in
            // place, swaps, shifts) go through temporary names first.
            std::set<std::wstring> sources;
            for (const auto& m : moves) sources.insert(ToLowerCopy(m.first));
            bool needTemp = false;
            for (const auto& m : moves) {
                std::wstring t = ToLowerCopy(m.second);
                if (t != ToLowerCopy(m.first) && sources.count(t)) needTemp = true;
            }
            std::vector<std::pair<std::wstring, std::wstring>> staged; // temp -> final, for phase two
            std::vector<std::pair<std::wstring, std::wstring>> origin;  // original -> final, parallel to staged
            auto renameOne = [&](const std::wstring& a, const std::wstring& b) {
                std::error_code ec;
                fs::rename(a, b, ec);
                if (ec) {
                    ++failed;
                    if (firstError.empty()) firstError = NarrowUtf8(fs::path(a).filename().wstring()) + ": " + ec.message();
                    return false;
                }
                return true;
            };
            if (needTemp) {
                int n = 0;
                for (const auto& m : moves) {
                    if (renameCancel_.load()) break;
                    std::wstring temp = (fs::path(m.first).parent_path() / (L".mvren" + std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(n++) + L".tmp")).wstring();
                    if (renameOne(m.first, temp)) {
                        staged.push_back({temp, m.second});
                        origin.push_back({m.first, m.second});
                    }
                }
                for (size_t i = 0; i < staged.size(); ++i) {
                    if (renameOne(staged[i].first, staged[i].second)) {
                        done.push_back(origin[i]);
                        MovePathMetadata(origin[i].first, origin[i].second);
                    } else {
                        // Put it back under its old name rather than leaving a temp file behind.
                        std::error_code ec;
                        fs::rename(staged[i].first, origin[i].first, ec);
                    }
                    renameCurrent_++;
                }
            } else {
                for (const auto& m : moves) {
                    if (renameCancel_.load()) break;
                    if (renameOne(m.first, m.second)) {
                        done.push_back(m);
                        MovePathMetadata(m.first, m.second);
                    }
                    renameCurrent_++;
                }
            }
            if (!done.empty()) {
                nlohmann::json log = {{"moves", nlohmann::json::array()}};
                for (const auto& m : done) log["moves"].push_back({NarrowUtf8(m.first), NarrowUtf8(m.second)});
                std::error_code ec;
                fs::create_directories(RenameLogDir(), ec);
                char stamp[40];
                snprintf(stamp, sizeof(stamp), "rename_%020llu.json", static_cast<unsigned long long>(GetTickCount64()));
                SYSTEMTIME st;
                GetSystemTime(&st);
                snprintf(stamp, sizeof(stamp), "rename_%04d%02d%02d%02d%02d%02d%03d.json", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
                std::ofstream out(fs::path(RenameLogDir()) / stamp, std::ios::binary | std::ios::trunc);
                if (out) out << log.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            }
        } catch (...) {
            firstError = "Unexpected error";
        }
        std::string message = std::string(renameCancel_ ? "Cancelled. " : "") + "Renamed " + std::to_string(done.size()) + " file(s).";
        if (failed) message += " " + std::to_string(failed) + " failed (" + firstError + ").";
        std::lock_guard<std::mutex> lock(renameMutex_);
        renameFinishedMessage_ = std::move(message);
        renameFinishedMoves_ = std::move(done);
        renameFinishedUndo_ = false;
        renameFinished_ = true;
        renameRunning_ = false;
    });
}

void AppState::StartUndoRename() {
    CancelRename();
    std::wstring logPath = latestRenameLog_;
    renameMessage.clear();
    if (logPath.empty()) {
        renameMessage = "Nothing to undo.";
        return;
    }
    renameCurrent_ = 0;
    renameTotal_ = 0;
    renameRunning_ = true;
    renameThread_ = std::thread([this, logPath]() {
        std::vector<std::pair<std::wstring, std::wstring>> restored; // as moved back: current -> original
        int failed = 0;
        try {
            if (diskCacheReady_.valid()) diskCacheReady_.wait();
            std::ifstream f(logPath, std::ios::binary);
            nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
            if (j.is_object() && j.contains("moves") && j["moves"].is_array()) {
                renameTotal_ = static_cast<int>(j["moves"].size());
                const auto& arr = j["moves"];
                for (size_t k = arr.size(); k-- > 0;) { // newest move first
                    const auto& m = arr[k];
                    renameCurrent_++;
                    if (!m.is_array() || m.size() != 2) continue;
                    std::wstring original = Utf8ToWideLocal(m[0].get<std::string>());
                    std::wstring current = Utf8ToWideLocal(m[1].get<std::string>());
                    std::error_code ec;
                    if (!fs::exists(current, ec) || (fs::exists(original, ec) && ToLowerCopy(original) != ToLowerCopy(current))) {
                        ++failed;
                        continue;
                    }
                    fs::rename(current, original, ec);
                    if (ec) {
                        ++failed;
                        continue;
                    }
                    restored.push_back({current, original});
                    MovePathMetadata(current, original);
                }
            }
            std::error_code ec;
            fs::path done = fs::path(logPath).replace_extension(L".undone.json");
            fs::rename(logPath, done, ec);
        } catch (...) {
        }
        std::string message = "Restored " + std::to_string(restored.size()) + " file name(s).";
        if (failed) message += " " + std::to_string(failed) + " could not be restored (moved or already in use).";
        std::lock_guard<std::mutex> lock(renameMutex_);
        renameFinishedMessage_ = std::move(message);
        renameFinishedMoves_ = std::move(restored);
        renameFinishedUndo_ = true;
        renameFinished_ = true;
        renameRunning_ = false;
    });
}

void AppState::PumpRename() {
    std::string message;
    std::vector<std::pair<std::wstring, std::wstring>> moves;
    {
        std::lock_guard<std::mutex> lock(renameMutex_);
        if (!renameFinished_) return;
        renameFinished_ = false;
        message = std::move(renameFinishedMessage_);
        moves = std::move(renameFinishedMoves_);
    }
    renameMessage = message;
    SetStatus(message);
    RefreshRenameLog();
    if (moves.empty() || currentFolder.empty()) return;
    // Re-list the folder and keep the same picture selected under its new name.
    std::wstring selected;
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(entries.size())) {
        selected = entries[selectedIndex].path;
        for (const auto& m : moves) {
            if (_wcsicmp(m.first.c_str(), selected.c_str()) == 0) {
                selected = m.second;
                break;
            }
        }
    }
    ReloadFolder(selected);
}

// ---------------------------------------------------------------------------
// "Already described with Vision" index
// ---------------------------------------------------------------------------

namespace {
std::wstring DescribedKey(const std::wstring& fileName, uintmax_t size) {
    return ToLowerCopy(fileName) + L"|" + std::to_wstring(size);
}
} // namespace

std::string AppState::VisionMaskLabel(int mask) {
    std::string out;
    auto add = [&](const char* s) {
        if (!out.empty()) out += ", ";
        out += s;
    };
    if (mask & 1) add("image prompt");
    if (mask & 2) add("tags");
    if (mask & 4) add("video scene");
    if (mask & 8) add("styled prompt");
    return out;
}

int AppState::VisionMaskFor(const ImageEntry& e) const {
    int mask = 0;
    auto it = describedByCopy_.find(DescribedKey(fs::path(e.path).filename().wstring(), e.size));
    if (it != describedByCopy_.end()) mask |= it->second;
    auto sit = sessionMask_.find(ToLowerCopy(e.path));
    if (sit != sessionMask_.end()) mask |= sit->second;
    return mask;
}

void AppState::PumpDescribed() {
    bool apply = false;
    {
        std::lock_guard<std::mutex> lock(describedMutex_);
        if (describedReady_) {
            describedByCopy_ = std::move(describedPending_);
            describedPending_.clear();
            describedReady_ = false;
            apply = true;
        }
    }
    if (apply) {
        for (ImageEntry& e : entries) e.visionMask = VisionMaskFor(e);
        filterEpoch++;
    }
    if (describedDirty_ && !describedRunning_ && GetTickCount64() - describedDirtyTick_ > 800) {
        describedDirty_ = false;
        if (describedThread_.joinable()) describedThread_.join();
        const std::wstring dir = WidenUtf8(visionOutputDir);
        describedRunning_ = true;
        describedThread_ = std::thread([this, dir]() {
            std::unordered_map<std::wstring, int> found;
            try {
                std::error_code ec;
                if (!dir.empty() && fs::is_directory(dir, ec)) {
                    std::unordered_map<std::wstring, std::vector<std::pair<std::wstring, uintmax_t>>> imagesByStem;
                    std::vector<std::pair<std::wstring, int>> results; // (stem without the result suffix, mask)
                    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                        std::error_code fe;
                        if (!it->is_regular_file(fe)) continue;
                        const fs::path& p = it->path();
                        const std::wstring folderKey = ToLowerCopy(p.parent_path().wstring()) + L"|";
                        if (IsSupportedImage(p)) {
                            imagesByStem[folderKey + ToLowerCopy(p.stem().wstring())].push_back({p.filename().wstring(), it->file_size(fe)});
                        } else if (ToLowerCopy(p.extension().wstring()) == L".txt") {
                            std::wstring stem = ToLowerCopy(p.stem().wstring());
                            int mask = 1;
                            const size_t styleAt = stem.rfind(L".style-");
                            if (styleAt != std::wstring::npos && stem.size() > styleAt + 7) {
                                mask = 8;
                                stem.erase(styleAt);
                            } else if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, L".tags") == 0) {
                                mask = 2;
                                stem.erase(stem.size() - 5);
                            } else {
                                size_t v = stem.rfind(L".video-");
                                if (v != std::wstring::npos && stem.size() > 1 && stem.back() == L's') {
                                    mask = 4;
                                    stem.erase(v);
                                }
                            }
                            results.push_back({folderKey + stem, mask});
                        }
                    }
                    for (const auto& [stem, mask] : results) {
                        auto img = imagesByStem.find(stem);
                        if (img == imagesByStem.end()) continue;
                        for (const auto& [name, size] : img->second) found[DescribedKey(name, size)] |= mask;
                    }
                }
            } catch (...) {
            }
            {
                std::lock_guard<std::mutex> lock(describedMutex_);
                describedPending_ = std::move(found);
                describedReady_ = true;
            }
            describedRunning_ = false;
        });
    }
}

// ---------------------------------------------------------------------------
// Vision library tab
// ---------------------------------------------------------------------------

void AppState::RequestVisionLibrary() {
    libWanted_ = true;
    if (!visionLibLoaded && !libScanning_ && !libDirty_) {
        libDirty_ = true;
        libDirtyTick_ = 0;
    }
}

void AppState::PumpVisionLibrary() {
    bool swapped = false;
    std::wstring keepSelected = visionLibSelected >= 0 && visionLibSelected < static_cast<int>(visionLib.size())
                                    ? visionLib[visionLibSelected].imagePath
                                    : std::wstring();
    {
        std::lock_guard<std::mutex> lock(libMutex_);
        if (libReady_) {
            visionLib = std::move(libPending_);
            libPending_.clear();
            visionLibDirs = std::move(libPendingDirs_);
            libPendingDirs_.clear();
            visionLibFolder = std::move(libPendingFolder_);
            libReady_ = false;
            visionLibLoaded = true;
            swapped = true;
        }
    }
    if (swapped) {
        ++visionLibVersion;
        visionLibSelected = -1;
        for (size_t i = 0; i < visionLib.size() && !keepSelected.empty(); ++i) {
            if (_wcsicmp(visionLib[i].imagePath.c_str(), keepSelected.c_str()) == 0) {
                visionLibSelected = static_cast<int>(i);
                break;
            }
        }
        if (visionLibSelected < 0) {
            ReleaseImage(visionLibImage);
            visionLibImagePath.clear();
        }
    }
    {
        // Keep preview memory bounded: drop everything once there are many (the visible ones reload).
        std::lock_guard<std::mutex> lock(libThumbMutex_);
        if (libThumbs_.size() > 700) {
            for (auto& [path, t] : libThumbs_) {
                if (t.srv) ReleaseAfterFrame(t.srv);
            }
            libThumbs_.clear();
            libThumbQueue_.clear();
        }
    }
    if (libWanted_ && libDirty_ && !libScanning_ && GetTickCount64() - libDirtyTick_ > 600) {
        libDirty_ = false;
        if (libThread_.joinable()) libThread_.join();
        const std::wstring dir = WidenUtf8(visionOutputDir);
        libScanning_ = true;
        libThread_ = std::thread([this, dir]() {
            std::vector<VisionLibItem> items;
            std::vector<std::string> dirsFound;
            try {
                std::error_code ec;
                if (!dir.empty() && fs::is_directory(dir, ec)) {
                    struct ImageInfo {
                        std::wstring path, name, dir;
                        uintmax_t size = 0;
                        fs::file_time_type mtime{};
                    };
                    std::unordered_map<std::wstring, ImageInfo> imagesByStem;
                    struct TextInfo {
                        std::wstring stem, path;
                        int kind = 0; // 0 prompt, 1 tags, 2 video
                        int seconds = 0;
                        std::filesystem::file_time_type mtime{};
                        std::string styleName; // kind 3: <name>.style-<style>.txt
                    };
                    std::vector<TextInfo> texts;
                    // everything below the output folder: the pictures sit in folders the user made
                    const std::wstring dirKey = ToLowerCopy(dir);
                    auto relDirOf = [&](const fs::path& p) {
                        std::wstring parent = p.parent_path().wstring();
                        return parent.size() > dir.size() + 1 ? parent.substr(dir.size() + 1) : std::wstring();
                    };
                    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                        std::error_code fe;
                        if (it->is_directory(fe)) {
                            dirsFound.push_back(NarrowUtf8(relDirOf(it->path() / L"x"))); // the folder itself, relative to the output folder
                            continue;
                        }
                        if (!it->is_regular_file(fe)) continue;
                        const fs::path& p = it->path();
                        const std::wstring folderKey = ToLowerCopy(p.parent_path().wstring()) + L"|";
                        if (IsSupportedImage(p)) {
                            ImageInfo info;
                            info.dir = relDirOf(p);
                            info.path = p.wstring();
                            info.name = p.filename().wstring();
                            info.size = it->file_size(fe);
                            info.mtime = it->last_write_time(fe);
                            imagesByStem[folderKey + ToLowerCopy(p.stem().wstring())] = std::move(info);
                        } else if (ToLowerCopy(p.extension().wstring()) == L".txt") {
                            TextInfo t;
                            t.path = p.wstring();
                            t.mtime = it->last_write_time(fe);
                            std::wstring stem = ToLowerCopy(p.stem().wstring());
                            const size_t styleAt = stem.rfind(L".style-");
                            if (styleAt != std::wstring::npos && stem.size() > styleAt + 7) {
                                t.kind = 3;
                                t.styleName = NarrowUtf8(stem.substr(styleAt + 7));
                                stem.erase(styleAt);
                            } else if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, L".tags") == 0) {
                                t.kind = 1;
                                stem.erase(stem.size() - 5);
                            } else {
                                size_t v = stem.rfind(L".video-");
                                if (v != std::wstring::npos && stem.back() == L's' && stem.size() > v + 8) {
                                    std::wstring digits = stem.substr(v + 7, stem.size() - v - 8);
                                    if (!digits.empty() && std::all_of(digits.begin(), digits.end(), ::iswdigit)) {
                                        t.kind = 2;
                                        t.seconds = std::stoi(digits);
                                        stem.erase(v);
                                    }
                                }
                            }
                            t.stem = folderKey + stem;
                            texts.push_back(std::move(t));
                        }
                    }
                    std::unordered_map<std::wstring, VisionLibItem> byStem;
                    auto readText = [](const std::wstring& path) {
                        std::ifstream f(path, std::ios::binary);
                        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                        if (s.size() > 400000) s.resize(400000);
                        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
                        return s;
                    };
                    for (const TextInfo& t : texts) {
                        auto img = imagesByStem.find(t.stem);
                        if (img == imagesByStem.end()) continue;
                        VisionLibItem& item = byStem[t.stem];
                        if (item.imagePath.empty()) {
                            item.imagePath = img->second.path;
                            item.name = img->second.name;
                            item.dir = NarrowUtf8(img->second.dir);
                            item.size = img->second.size;
                            item.mtime = img->second.mtime;
                        }
                        if (t.mtime > item.resultTime) item.resultTime = t.mtime;
                        std::string text = readText(t.path);
                        if (t.kind == 0) item.prompt = std::move(text);
                        else if (t.kind == 1) item.tags = std::move(text);
                        else if (t.kind == 3) item.styled.push_back({t.styleName, std::move(text)});
                        else item.videos.push_back({t.seconds, std::move(text)});
                    }
                    for (auto& [stem, item] : byStem) {
                        std::sort(item.videos.begin(), item.videos.end());
                        std::sort(item.styled.begin(), item.styled.end());
                        std::string all = NarrowUtf8(item.name) + "\n" + item.prompt + "\n" + item.tags;
                        for (const auto& v : item.videos) all += "\n" + v.second;
                        for (const auto& st : item.styled) all += "\n" + st.first + "\n" + st.second;
                        std::transform(all.begin(), all.end(), all.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        item.searchLower = std::move(all);
                        items.push_back(std::move(item));
                    }
                    // newest result first: describing an old image again puts it at the top
                    std::sort(items.begin(), items.end(), [](const VisionLibItem& a, const VisionLibItem& b) { return a.resultTime > b.resultTime; });
                }
            } catch (...) {
            }
            {
                std::lock_guard<std::mutex> lock(libMutex_);
                libPending_ = std::move(items);
                std::sort(dirsFound.begin(), dirsFound.end());
                dirsFound.erase(std::unique(dirsFound.begin(), dirsFound.end()), dirsFound.end());
                libPendingDirs_ = std::move(dirsFound);
                libPendingFolder_ = NarrowUtf8(dir);
                libReady_ = true;
            }
            libScanning_ = false;
        });
    }
}

void AppState::SelectVisionLib(int index) {
    if (index < 0 || index >= static_cast<int>(visionLib.size())) return;
    visionLibSelected = index;
    const std::wstring& path = visionLib[index].imagePath;
    if (_wcsicmp(path.c_str(), visionLibImagePath.c_str()) == 0 && visionLibImage.srv) return;
    ReleaseImage(visionLibImage);
    visionLibImagePath = path;
    std::string err;
    LoadImageTexture(device_, path, visionLibImage, &err);
}

// The file's last-write time and size in one number (0 = not there)
static unsigned long long FileStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    const unsigned long long t = (static_cast<unsigned long long>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    const unsigned long long size = (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    return t ^ (size * 0x9E3779B97F4A7C15ull);
}

AppState::LibThumb AppState::LibraryThumb(const std::wstring& imagePath) {
    LibThumb t = LibraryThumbRaw(imagePath);
    if (t.srv && ShouldBlur(imagePath)) t.srv = BlurredCopy(device_, t.srv); // (whichever page draws it)
    return t;
}

AppState::LibThumb AppState::LibraryThumbRaw(const std::wstring& imagePath) {
    const std::wstring key = ToLowerCopy(imagePath);
    {
        std::lock_guard<std::mutex> lock(libThumbMutex_);
        auto it = libThumbs_.find(key);
        if (it != libThumbs_.end()) {
            // every 2 s: is it still the same file? (ComfyUI names the next video like a deleted one: H3_Swap_00001_ again)
            const unsigned long long now = GetTickCount64();
            if ((it->second.stamp != 0 || it->second.failed) && now - it->second.checkedMs > 2000) {
                it->second.checkedMs = now;
                const unsigned long long stamp = FileStamp(imagePath);
                if (stamp != 0 && stamp != it->second.stamp) {
                    if (it->second.srv) ReleaseAfterFrame(it->second.srv);
                    it->second = LibThumb{};
                    libThumbQueue_.push_back(imagePath); // loaded again; the old picture is gone meanwhile
                    libThumbCv_.notify_one();
                    return LibThumb{};
                }
            }
            return it->second;
        }
        libThumbs_[key] = LibThumb{}; // placeholder so it is only queued once
        libThumbQueue_.push_back(imagePath);
    }
    libThumbCv_.notify_one();
    if (!libThumbThread_.joinable()) {
        libThumbThread_ = std::thread([this]() {
            for (;;) {
                std::wstring path;
                {
                    std::unique_lock<std::mutex> lock(libThumbMutex_);
                    libThumbCv_.wait(lock, [this]() { return libThumbStop_ || !libThumbQueue_.empty(); });
                    if (libThumbStop_) return;
                    path = std::move(libThumbQueue_.front());
                    libThumbQueue_.pop_front();
                }
                LoadedImage thumb;
                std::string err;
                bool ok = false;
                const unsigned long long stamp = FileStamp(path);
                try {
                    ok = LoadImageThumbnail(device_, path, 192, thumb, nullptr, &err);
                } catch (...) {
                }
                std::lock_guard<std::mutex> lock(libThumbMutex_);
                LibThumb& t = libThumbs_[ToLowerCopy(path)];
                if (t.srv) ReleaseAfterFrame(t.srv);
                t.srv = ok ? thumb.srv : nullptr;
                t.w = ok ? thumb.width : 0;
                t.h = ok ? thumb.height : 0;
                t.failed = !ok;
                t.stamp = stamp;
                t.checkedMs = GetTickCount64();
            }
        });
    }
    return LibThumb{};
}

void AppState::StopLibraryThumbs() {
    {
        std::lock_guard<std::mutex> lock(libThumbMutex_);
        libThumbStop_ = true;
        libThumbQueue_.clear();
    }
    libThumbCv_.notify_all();
    if (libThumbThread_.joinable()) libThumbThread_.join();
    std::lock_guard<std::mutex> lock(libThumbMutex_);
    for (auto& [path, t] : libThumbs_) {
        if (t.srv) ReleaseAfterFrame(t.srv);
    }
    libThumbs_.clear();
}
