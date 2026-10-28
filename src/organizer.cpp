#include "organizer.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <windows.h>

#include "metadata.h"

namespace fs = std::filesystem;

namespace {

enum class Kind { Image, Video, Sidecar, Other, Junk };

const std::set<std::wstring> kImageExts = {L".png", L".jpg", L".jpeg", L".jfif", L".webp", L".bmp", L".gif"};
const std::set<std::wstring> kVideoExts = {L".mp4", L".webm", L".mov", L".mkv", L".avi", L".m4v", L".wmv"};
const std::set<std::wstring> kSidecarExts = {L".txt", L".json", L".caption", L".tags", L".yaml", L".yml"};
// Explorer/macOS litter that shouldn't keep an otherwise-empty folder alive.
const std::set<std::wstring> kJunkNames = {L"thumbs.db", L"desktop.ini", L".ds_store"};
// Exact copies are moved here (flat, at the destination root) for review.
const wchar_t kDuplicatesDir[] = L"_duplicates";

const std::set<std::wstring> kClassNames = {L"sfw",       L"nsfw",          L"questionable",       L"unsorted",
                                            L"other",     L"video-sfw",     L"video-nsfw",         L"video-questionable",
                                            L"video-unsorted"};

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// Lowercased, backslash-separated, no trailing separator: comparable key for paths.
std::wstring Key(const fs::path& p) {
    std::wstring s = Lower(p.lexically_normal().wstring());
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

bool IsSameOrUnder(const std::wstring& key, const std::wstring& dirKey) {
    return key == dirKey || (key.size() > dirKey.size() && key.compare(0, dirKey.size(), dirKey) == 0 &&
                             (key[dirKey.size()] == L'\\' || dirKey.back() == L'\\'));
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

// Local-time year of the file's last-modified date (0 if unreadable).
int ModifiedYear(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data)) return 0;
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&data.ftLastWriteTime, &local) || !FileTimeToSystemTime(&local, &st)) return 0;
    return st.wYear;
}

bool IsYearName(const std::wstring& s) {
    return s.size() == 4 && std::all_of(s.begin(), s.end(), iswdigit);
}

fs::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

std::wstring LogDir() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = (len > 0 && len < MAX_PATH) ? buf : L".";
    return base + L"\\Try2Create\\logs";
}

// "name.png" -> "name (2).png", "name (3).png", ... until it doesn't exist.
fs::path UniqueDestination(const fs::path& dir, const std::wstring& fileName) {
    fs::path candidate = dir / fileName;
    std::error_code ec;
    fs::path fn(fileName);
    for (int n = 2; fs::exists(candidate, ec); ++n) {
        candidate = dir / (fn.stem().wstring() + L" (" + std::to_wstring(n) + L")" + fn.extension().wstring());
    }
    return candidate;
}

// Deletes `dir` if it's empty apart from junk files (which are deleted too).
bool RemoveIfEmpty(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> junk;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (it->is_regular_file(fec) && kJunkNames.count(Lower(it->path().filename().wstring()))) {
            junk.push_back(it->path());
        } else {
            return false;
        }
    }
    if (ec) return false;
    for (auto& j : junk) {
        SetFileAttributesW(j.c_str(), FILE_ATTRIBUTE_NORMAL); // desktop.ini is usually hidden+system
        DeleteFileW(j.c_str());
    }
    return RemoveDirectoryW(dir.c_str()) != 0;
}

struct FileRec {
    fs::path path;
    fs::path relParent; // parent folder relative to the source root ("" = root itself)
    fs::file_time_type mtime{};
    uintmax_t size = 0;
    Kind kind = Kind::Other;
    int year = 0;
    // Media only:
    float promptScore = -1.0f;
    float modelScore = -1.0f;
    std::vector<size_t> sidecars; // indices into the file list
    std::wstring sidecarSuffix;   // sidecars only: part of the name after the owner's stem (".txt", ".png.txt")
};

struct PlannedMove {
    size_t file;
    fs::path dstDir;
    int cls; // 0 sfw, 1 questionable, 2 nsfw, 3 unsorted, 4 other
};

const wchar_t* kClassDir[] = {L"sfw", L"questionable", L"nsfw", L"unsorted", L"other", kDuplicatesDir};

// Byte-for-byte comparison of two files (callers have already matched sizes).
// Different images almost always diverge in the first chunk, so this is cheap
// for near misses and only reads both files fully for true duplicates.
bool SameContents(const fs::path& a, const fs::path& b) {
    if (Key(a) == Key(b)) return false; // the same file isn't a duplicate of itself
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    std::vector<char> ba(1 << 20), bb(1 << 20);
    while (true) {
        fa.read(ba.data(), ba.size());
        fb.read(bb.data(), bb.size());
        std::streamsize na = fa.gcount(), nb = fb.gcount();
        if (na != nb || std::memcmp(ba.data(), bb.data(), static_cast<size_t>(na)) != 0) return false;
        if (na == 0 || !fa || !fb) return na == nb && fa.eof() == fb.eof();
    }
}

} // namespace

OrganizeResult RunOrganize(const OrganizeOptions& opts, NsfwClassifier& classifier, const OrganizeHooks& hooks,
                           OrganizeProgress& progress) {
    OrganizeResult result;
    std::error_code ec;
    const fs::path srcRoot = fs::absolute(opts.sourceRoot, ec).lexically_normal();
    const fs::path dstRoot =
        opts.destRoot.empty() ? srcRoot : fs::absolute(opts.destRoot, ec).lexically_normal();
    if (!fs::is_directory(srcRoot, ec)) {
        result.message = "Folder to organize doesn't exist.";
        return result;
    }
    const std::wstring srcKey = Key(srcRoot), dstKey = Key(dstRoot);
    const bool inPlace = srcKey == dstKey;

    // Folders never touched: user excludes, this app's own folder, a separate
    // destination nested inside the source, and system folders.
    std::vector<std::wstring> excludedKeys;
    for (const auto& ex : opts.excludes) {
        std::wstring trimmed = ex;
        trimmed.erase(0, trimmed.find_first_not_of(L" \t\\/"));
        while (!trimmed.empty() && (trimmed.back() == L' ' || trimmed.back() == L'\\' || trimmed.back() == L'/'))
            trimmed.pop_back();
        if (!trimmed.empty()) excludedKeys.push_back(Key(srcRoot / trimmed));
    }
    // Only when it's inside the tree: if the app lives above the source
    // folder, excluding it would exclude everything.
    const std::wstring exeKey = Key(ExeDir());
    if (exeKey != srcKey && IsSameOrUnder(exeKey, srcKey)) excludedKeys.push_back(exeKey);
    // A destination nested inside the source must not be rescanned. (The
    // reverse, a source inside the destination such as G:\AI Images\output
    // -> G:\AI Images, is fine and must not exclude anything.)
    if (!inPlace && IsSameOrUnder(dstKey, srcKey)) excludedKeys.push_back(dstKey);
    excludedKeys.push_back(Key(dstRoot / kDuplicatesDir)); // don't re-sort what was set aside
    auto isExcluded = [&](const fs::path& dir) {
        std::wstring k = Key(dir);
        std::wstring name = Lower(dir.filename().wstring());
        if (!name.empty() && (name[0] == L'$' || name == L"system volume information")) return true;
        return std::any_of(excludedKeys.begin(), excludedKeys.end(),
                           [&](const std::wstring& ex) { return IsSameOrUnder(k, ex); });
    };

    // ---- 1. Scan ---------------------------------------------------------
    progress.phase = kOrgScanning;
    std::vector<FileRec> files;
    std::vector<fs::path> visitedDirs;
    std::vector<fs::path> excludedDirs;
    for (fs::recursive_directory_iterator it(srcRoot, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (progress.cancel) break;
        std::error_code fec;
        if (it->is_directory(fec)) {
            if (it->is_symlink(fec) || isExcluded(it->path())) {
                excludedDirs.push_back(it->path());
                it.disable_recursion_pending();
            } else {
                visitedDirs.push_back(it->path());
            }
            continue;
        }
        if (!it->is_regular_file(fec)) continue;
        FileRec f;
        f.path = it->path();
        f.relParent = f.path.parent_path().lexically_relative(srcRoot);
        if (f.relParent == L".") f.relParent.clear();
        f.size = it->file_size(fec);
        f.mtime = it->last_write_time(fec);
        std::wstring ext = Lower(f.path.extension().wstring());
        std::wstring name = Lower(f.path.filename().wstring());
        if (kJunkNames.count(name)) f.kind = Kind::Junk;
        else if (kImageExts.count(ext)) f.kind = Kind::Image;
        else if (kVideoExts.count(ext)) f.kind = opts.includeVideos ? Kind::Video : Kind::Other;
        else f.kind = Kind::Other;
        files.push_back(std::move(f));
    }
    if (progress.cancel) {
        result.message = "Cancelled.";
        return result;
    }

    // Attach caption/metadata sidecars to the image or video they belong to:
    // "img.txt" or "img.png.txt" next to "img.png".
    std::unordered_map<std::wstring, size_t> mediaByStem; // "<dir key>|<lower stem>" -> file index
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].kind == Kind::Image || files[i].kind == Kind::Video) {
            mediaByStem[Key(files[i].path.parent_path()) + L"|" + Lower(files[i].path.stem().wstring())] = i;
        }
    }
    for (size_t i = 0; i < files.size(); ++i) {
        FileRec& f = files[i];
        if (f.kind != Kind::Other || !kSidecarExts.count(Lower(f.path.extension().wstring()))) continue;
        std::wstring dirKey = Key(f.path.parent_path()) + L"|";
        fs::path stem = f.path.stem(); // "img" or "img.png"
        auto found = mediaByStem.find(dirKey + Lower(stem.wstring()));
        if (found == mediaByStem.end() && !stem.extension().empty()) {
            found = mediaByStem.find(dirKey + Lower(stem.stem().wstring()));
        }
        if (found == mediaByStem.end()) continue;
        f.kind = Kind::Sidecar;
        f.sidecarSuffix = f.path.filename().wstring().substr(files[found->second].path.stem().wstring().size());
        files[found->second].sidecars.push_back(i);
    }

    std::vector<size_t> media;
    for (size_t i = 0; i < files.size(); ++i) {
        files[i].year = ModifiedYear(files[i].path);
        if (files[i].kind == Kind::Image || files[i].kind == Kind::Video) media.push_back(i);
    }

    // ---- 2. Score --------------------------------------------------------
    std::string warning;
    NsfwSource source = opts.source;
    bool useModel = source != NsfwSource::Prompt;
    for (size_t i : media) {
        FileRec& f = files[i];
        if (hooks.cachedScore) f.modelScore = hooks.cachedScore(f.path.wstring(), f.mtime, f.size);
    }
    bool needModel = useModel && std::any_of(media.begin(), media.end(), [&](size_t i) { return files[i].modelScore < 0; });
    if (needModel) {
        progress.phase = kOrgLoadingModel;
        std::string err;
        if (!classifier.EnsureLoaded(&err)) {
            if (source == NsfwSource::ImageModel) {
                result.message = "Image classifier unavailable: " + err;
                return result;
            }
            warning = "Image classifier unavailable (" + err + "); used prompt keywords only. ";
            source = NsfwSource::Prompt;
            useModel = needModel = false;
        }
    }
    bool haveFfmpeg = NsfwClassifier::FfmpegAvailable();
    if (needModel && opts.includeVideos && !haveFfmpeg) warning += "ffmpeg not found, so videos went to 'unsorted'. ";

    progress.phase = kOrgScoring;
    progress.current = 0;
    progress.total = static_cast<int>(media.size());
    std::atomic<size_t> next{0};
    std::atomic<int> scoreFailures{0};
    auto worker = [&]() {
        for (size_t n = next++; n < media.size() && !progress.cancel; n = next++) {
            FileRec& f = files[media[n]];
            try {
                if (f.kind == Kind::Image && source != NsfwSource::ImageModel) {
                    ImageMetadata md = ReadImageMetadata(f.path.wstring());
                    if (!md.parsed.positivePrompt.empty()) f.promptScore = PromptNsfwScore(md.parsed.positivePrompt);
                }
                if (needModel && f.modelScore < 0) {
                    if (f.kind == Kind::Image) f.modelScore = classifier.ScoreFile(f.path.wstring(), nullptr);
                    else if (haveFfmpeg) f.modelScore = classifier.ScoreVideo(f.path.wstring(), nullptr);
                    if (f.modelScore >= 0 && hooks.storeScore) hooks.storeScore(f.path.wstring(), f.mtime, f.size, f.modelScore);
                    if (f.modelScore < 0) scoreFailures++;
                }
            } catch (...) {
                scoreFailures++;
            }
            progress.current++;
        }
    };
    unsigned threadCount = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threadCount; ++t) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
    if (progress.cancel) {
        result.message = warning + "Cancelled while scoring; no files were moved (scores so far are cached).";
        return result;
    }

    // ---- 3. Plan ---------------------------------------------------------
    // Original subfolders are kept under <year>\<class>\. When organizing in
    // place, a file already inside <year>\<class>\ has that prefix stripped
    // so re-running (e.g. with a new threshold) doesn't nest folders deeper.
    auto subPath = [&](const fs::path& rel) {
        if (!opts.keepSubfolders) return fs::path(); // flat: <year>\<class>\<file>
        std::vector<fs::path> parts(rel.begin(), rel.end());
        size_t skip = 0;
        if (inPlace && parts.size() >= 2 && IsYearName(parts[0].wstring()) && kClassNames.count(Lower(parts[1].wstring())))
            skip = 2;
        fs::path out;
        for (size_t i = skip; i < parts.size(); ++i) out /= parts[i];
        return out;
    };
    auto yearDir = [](int year) { return year > 0 ? std::to_wstring(year) : std::wstring(L"unknown year"); };

    std::vector<PlannedMove> plan;
    int classCounts[6] = {0, 0, 0, 0, 0, 0};
    std::map<int, int> yearCounts;
    int alreadyInPlace = 0, videoCount = 0, sidecarCount = 0, otherCount = 0, duplicateCount = 0;
    std::vector<bool> staying(files.size(), true);

    // Duplicate detection: files already in the collection (or kept earlier
    // in this run) indexed by size; a same-size candidate is then compared
    // byte for byte, so only true copies match.
    progress.phase = kOrgDuplicates;
    std::unordered_map<uintmax_t, std::vector<fs::path>> bySize;
    auto isDuplicate = [&](const FileRec& f) {
        auto it = bySize.find(f.size);
        if (it == bySize.end()) return false;
        return std::any_of(it->second.begin(), it->second.end(),
                           [&](const fs::path& p) { return SameContents(f.path, p); });
    };
    if (opts.detectDuplicates && !inPlace) {
        // Organizing into a separate collection: index its <year>\<class>
        // folders (skipping the source, if it lives inside the collection).
        for (fs::directory_iterator y(dstRoot, ec), yEnd; !ec && y != yEnd; y.increment(ec)) {
            std::error_code dec;
            if (!y->is_directory(dec) || !IsYearName(y->path().filename().wstring())) continue;
            for (fs::directory_iterator c(y->path(), dec), cEnd; !dec && c != cEnd; c.increment(dec)) {
                std::error_code cec;
                if (!c->is_directory(cec) || !kClassNames.count(Lower(c->path().filename().wstring()))) continue;
                for (fs::recursive_directory_iterator it(c->path(), fs::directory_options::skip_permission_denied, cec), end;
                     !cec && it != end; it.increment(cec)) {
                    std::error_code fec;
                    if (!it->is_regular_file(fec) || IsSameOrUnder(Key(it->path()), srcKey)) continue;
                    std::wstring ext = Lower(it->path().extension().wstring());
                    if (!kImageExts.count(ext) && !kVideoExts.count(ext)) continue;
                    bySize[it->file_size(fec)].push_back(it->path());
                }
            }
        }
    }

    struct Placement {
        size_t file;
        int cls;
        fs::path dstDir;
    };
    std::vector<Placement> inPlaceItems, moveItems;
    for (size_t i : media) {
        FileRec& f = files[i];
        float score = source == NsfwSource::ImageModel ? f.modelScore
                      : source == NsfwSource::Prompt   ? f.promptScore
                                                       : std::max(f.modelScore, f.promptScore);
        int cls = score < 0                                                   ? 3
                  : score >= opts.threshold                                   ? 2
                  : (opts.useQuestionable && score >= opts.questionableLow) ? 1
                                                                              : 0;
        yearCounts[f.year]++;
        std::wstring classDir = (f.kind == Kind::Video ? L"video-" : L"") + std::wstring(kClassDir[cls]);
        fs::path dstDir = dstRoot / yearDir(f.year) / classDir / subPath(f.relParent);
        (Key(dstDir) == Key(f.path.parent_path()) ? inPlaceItems : moveItems).push_back({i, cls, dstDir});
    }

    // Files already sorted are processed first so they're the copies that
    // stay; a copy found among them (from an earlier run) is moved out too.
    const fs::path duplicatesDir = dstRoot / kDuplicatesDir;
    auto place = [&](const Placement& p, bool alreadyThere) {
        FileRec& f = files[p.file];
        if (opts.detectDuplicates && isDuplicate(f)) {
            duplicateCount++;
            plan.push_back({p.file, duplicatesDir, 5});
        } else {
            classCounts[p.cls]++;
            if (opts.detectDuplicates) bySize[f.size].push_back(f.path);
            if (alreadyThere) {
                alreadyInPlace++;
                return;
            }
            plan.push_back({p.file, p.dstDir, p.cls});
            if (f.kind == Kind::Video) videoCount++;
        }
        staying[p.file] = false;
        for (size_t s : f.sidecars) {
            staying[s] = false;
            sidecarCount++;
        }
    };
    progress.current = 0;
    progress.total = static_cast<int>(media.size());
    for (auto& p : inPlaceItems) {
        place(p, true);
        progress.current++;
    }
    for (auto& p : moveItems) {
        place(p, false);
        progress.current++;
    }
    if (opts.moveOtherFiles) {
        for (size_t i = 0; i < files.size(); ++i) {
            FileRec& f = files[i];
            if (f.kind != Kind::Other || f.relParent.empty()) continue; // root-level files are left alone
            fs::path dstDir = dstRoot / yearDir(f.year) / kClassDir[4] / subPath(f.relParent);
            if (Key(dstDir) == Key(f.path.parent_path())) continue;
            plan.push_back({i, dstDir, 4});
            staying[i] = false;
            otherCount++;
        }
    }
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].kind == Kind::Junk) staying[i] = false; // deleted along with its empty folder
    }

    auto summary = [&](const char* verb) {
        std::string s = std::string(verb) + " " + std::to_string(plan.size() + sidecarCount) + " file(s)";
        s += " (" + std::to_string(plan.size() - videoCount - otherCount - duplicateCount) + " images, " +
             std::to_string(videoCount) +
             " videos, " + std::to_string(sidecarCount) + " caption sidecars, " + std::to_string(otherCount) +
             " other)";
        if (alreadyInPlace) s += ", " + std::to_string(alreadyInPlace) + " already in place";
        if (duplicateCount) {
            s += ".\nDuplicates (exact copies of a file already kept): " + std::to_string(duplicateCount) + " -> " +
                 ToUtf8(duplicatesDir.wstring());
        }
        s += ".\nNSFW " + std::to_string(classCounts[2]) +
             (opts.useQuestionable ? ", questionable " + std::to_string(classCounts[1]) : std::string()) +
             ", SFW " + std::to_string(classCounts[0]) + ", unsorted (no score) " + std::to_string(classCounts[3]) +
             ".\nBy year:";
        for (auto& [y, n] : yearCounts) s += " " + (y > 0 ? std::to_string(y) : std::string("unknown")) + ": " + std::to_string(n) + ";";
        s += "\nSkipped folders: " + std::to_string(excludedDirs.size()) + ".";
        return s;
    };

    // ---- Dry run: report what would happen, including which folders would
    // end up empty and be removed.
    if (opts.dryRun) {
        std::set<std::wstring> keep;
        auto keepUp = [&](fs::path d) {
            while (true) {
                std::wstring k = Key(d);
                if (!IsSameOrUnder(k, srcKey) || !keep.insert(k).second) break;
                d = d.parent_path();
            }
        };
        for (size_t i = 0; i < files.size(); ++i)
            if (staying[i]) keepUp(files[i].path.parent_path());
        for (auto& d : excludedDirs) keepUp(d);
        if (!opts.copyInsteadOfMove) {
            for (auto& m : plan) keepUp(m.dstDir);
        }
        int wouldRemove = 0;
        if (!opts.copyInsteadOfMove && opts.removeEmptyFolders) {
            for (auto& d : visitedDirs)
                if (!keep.count(Key(d))) wouldRemove++;
        }
        result.message = warning + "PREVIEW (nothing changed): " +
                         summary(opts.copyInsteadOfMove ? "would copy" : "would move") +
                         (opts.copyInsteadOfMove ? std::string()
                                                 : "\n" + std::to_string(wouldRemove) + " folder(s) would be removed as empty.");
        if (scoreFailures) result.message += "\n" + std::to_string(scoreFailures.load()) + " file(s) couldn't be scored (-> unsorted).";
        return result;
    }

    // ---- 4. Move ---------------------------------------------------------
    progress.phase = kOrgMoving;
    progress.current = 0;
    progress.total = static_cast<int>(plan.size());
    fs::create_directories(LogDir(), ec);
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04d%02d%02d_%02d%02d%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
             now.wSecond);
    std::wstring logPath = LogDir() + L"\\organize_" + stamp + L".tsv";
    std::ofstream log(logPath, std::ios::binary | std::ios::trunc);
    log << "# organize\t" << ToUtf8(srcRoot.wstring()) << "\t" << ToUtf8(dstRoot.wstring()) << "\n";

    const char op = opts.copyInsteadOfMove ? 'C' : 'M';
    int done = 0, failures = 0;
    std::vector<std::string> failureSamples;
    auto transfer = [&](const fs::path& from, const fs::path& to) {
        BOOL ok = opts.copyInsteadOfMove ? CopyFileW(from.c_str(), to.c_str(), TRUE)
                                         : MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED);
        if (!ok) {
            failures++;
            if (failureSamples.size() < 3) failureSamples.push_back(ToUtf8(from.filename().wstring()));
            return false;
        }
        log << op << "\t" << ToUtf8(from.wstring()) << "\t" << ToUtf8(to.wstring()) << "\n";
        log.flush(); // keep the log usable for undo even if we crash mid-run
        done++;
        return true;
    };

    bool cancelled = false;
    for (const PlannedMove& m : plan) {
        if (progress.cancel) {
            cancelled = true;
            break;
        }
        progress.current++;
        FileRec& f = files[m.file];
        fs::create_directories(m.dstDir, ec);
        fs::path dst = UniqueDestination(m.dstDir, f.path.filename().wstring());
        if (!transfer(f.path, dst)) continue;
        if (hooks.renameScore && !opts.copyInsteadOfMove && f.kind != Kind::Other) {
            hooks.renameScore(f.path.wstring(), dst.wstring());
        }
        // Sidecars follow under the media file's (possibly de-duplicated) new stem.
        for (size_t s : f.sidecars) {
            fs::path sDst = UniqueDestination(m.dstDir, dst.stem().wstring() + files[s].sidecarSuffix);
            transfer(files[s].path, sDst);
        }
    }
    log.close();
    result.filesChanged = done > 0;

    // ---- 5. Remove folders left empty (deepest first) ----------------------
    int removed = 0;
    if (!opts.copyInsteadOfMove && opts.removeEmptyFolders) {
        progress.phase = kOrgCleaning;
        std::sort(visitedDirs.begin(), visitedDirs.end(), [](const fs::path& a, const fs::path& b) {
            return a.wstring().size() > b.wstring().size();
        });
        for (auto& d : visitedDirs) {
            if (RemoveIfEmpty(d)) removed++;
        }
        result.filesChanged = result.filesChanged || removed > 0;
    }

    if (done == 0) {
        DeleteFileW(logPath.c_str()); // nothing to undo
    }
    std::string msg = warning;
    if (cancelled) msg += "Cancelled partway; finished moves are logged and can be undone. ";
    msg += summary(opts.copyInsteadOfMove ? "Copied" : "Moved");
    if (!opts.copyInsteadOfMove && opts.removeEmptyFolders) msg += "\nRemoved " + std::to_string(removed) + " empty folder(s).";
    if (failures) {
        msg += "\n" + std::to_string(failures) + " file(s) couldn't be moved (in use?), e.g.";
        for (auto& s : failureSamples) msg += " " + s;
    }
    if (scoreFailures) msg += "\n" + std::to_string(scoreFailures.load()) + " file(s) couldn't be scored (-> unsorted).";
    if (done > 0 && !opts.copyInsteadOfMove) msg += "\nUse \"Undo last organize\" to put everything back.";
    result.message = msg;
    return result;
}

std::wstring LatestOrganizeLog() {
    std::wstring best;
    std::error_code ec;
    for (fs::directory_iterator it(LogDir(), ec), end; !ec && it != end; it.increment(ec)) {
        std::wstring name = it->path().filename().wstring();
        if (name.rfind(L"organize_", 0) != 0 || it->path().extension() != L".tsv") continue;
        if (name.find(L".undone") != std::wstring::npos) continue;
        if (it->path().wstring() > best) best = it->path().wstring(); // timestamped names sort chronologically
    }
    return best;
}

OrganizeResult UndoOrganize(const std::wstring& logPath, const OrganizeHooks& hooks, OrganizeProgress& progress) {
    OrganizeResult result;
    std::ifstream in(logPath, std::ios::binary);
    if (!in) {
        result.message = "No organize log to undo.";
        return result;
    }
    std::wstring dstRootKey;
    std::vector<std::pair<std::wstring, std::wstring>> moves; // original, moved-to
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t t1 = line.find('\t');
        size_t t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        std::string a = line.substr(0, t1), b = line.substr(t1 + 1, t2 - t1 - 1), c = line.substr(t2 + 1);
        if (a == "# organize") dstRootKey = Key(FromUtf8(c));
        else if (a == "M") moves.push_back({FromUtf8(b), FromUtf8(c)});
    }
    in.close();
    if (moves.empty()) {
        result.message = "That organize run has no moves to undo (copy runs leave the originals in place).";
        return result;
    }

    progress.phase = kOrgUndoing;
    progress.current = 0;
    progress.total = static_cast<int>(moves.size());
    int restored = 0, failures = 0;
    std::set<std::wstring> touchedDirs;
    std::error_code ec;
    for (auto it = moves.rbegin(); it != moves.rend(); ++it) {
        progress.current++;
        const fs::path original(it->first), moved(it->second);
        if (!fs::exists(moved, ec) || fs::exists(original, ec)) {
            failures++;
            continue;
        }
        fs::create_directories(original.parent_path(), ec);
        if (MoveFileExW(moved.c_str(), original.c_str(), MOVEFILE_COPY_ALLOWED)) {
            restored++;
            touchedDirs.insert(moved.parent_path().wstring());
            if (hooks.renameScore) hooks.renameScore(moved.wstring(), original.wstring());
        } else {
            failures++;
        }
    }

    // Remove the <year>\<class>\... folders the undo emptied, walking up
    // until a non-empty folder or the organize destination root.
    int removed = 0;
    std::vector<std::wstring> dirs(touchedDirs.begin(), touchedDirs.end());
    std::sort(dirs.begin(), dirs.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
    for (auto& d : dirs) {
        fs::path p(d);
        while (!dstRootKey.empty() && IsSameOrUnder(Key(p), dstRootKey) && Key(p) != dstRootKey && RemoveIfEmpty(p)) {
            removed++;
            p = p.parent_path();
        }
    }

    fs::path undonePath = fs::path(logPath).replace_extension(L".undone.tsv");
    fs::rename(logPath, undonePath, ec);
    result.filesChanged = restored > 0;
    result.message = "Undo: restored " + std::to_string(restored) + " file(s), removed " + std::to_string(removed) +
                     " emptied folder(s)." +
                     (failures ? " " + std::to_string(failures) + " couldn't be restored (missing, or the original path is taken)."
                               : std::string());
    return result;
}
