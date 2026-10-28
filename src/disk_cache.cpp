#include "disk_cache.h"
#include "image_texture.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <windows.h>
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string WideToUtf8Local(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWideLocal(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

const char* GeneratorToStr(Generator g) {
    switch (g) {
        case Generator::Automatic1111: return "Automatic1111";
        case Generator::ComfyUI: return "ComfyUI";
        case Generator::NovelAI: return "NovelAI";
        case Generator::InvokeAI: return "InvokeAI";
        case Generator::Generic: return "Generic";
        default: return "Unknown";
    }
}

Generator GeneratorFromStr(const std::string& s) {
    if (s == "Automatic1111") return Generator::Automatic1111;
    if (s == "ComfyUI") return Generator::ComfyUI;
    if (s == "NovelAI") return Generator::NovelAI;
    if (s == "InvokeAI") return Generator::InvokeAI;
    if (s == "Generic") return Generator::Generic;
    return Generator::Unknown;
}

json MetadataToJson(const ImageMetadata& m) {
    json j;
    j["hasMetadata"] = m.hasMetadata;
    j["generator"] = GeneratorToStr(m.generator);
    j["rawText"] = m.rawText;
    j["error"] = m.error;
    j["rev"] = m.parserRev;
    if (m.slim) j["slim"] = true;
    if (m.workflowMask) j["wfmask"] = m.workflowMask;
    if (m.isVideo) {
        j["video"] = true;
        j["duration"] = m.durationSec;
        j["vw"] = m.videoW;
        j["vh"] = m.videoH;
    }

    json rawFields = json::array();
    for (auto& f : m.rawFields) rawFields.push_back({{"key", f.key}, {"value", f.value}});
    j["rawFields"] = std::move(rawFields);

    j["positivePrompt"] = m.parsed.positivePrompt;
    j["negativePrompt"] = m.parsed.negativePrompt;
    j["tags"] = m.parsed.tags;
    j["negativeTags"] = m.parsed.negativeTags;

    json params = json::array();
    for (auto& kv : m.parsed.params) params.push_back({{"key", kv.first}, {"value", kv.second}});
    j["params"] = std::move(params);
    return j;
}

ImageMetadata MetadataFromJson(const json& j) {
    ImageMetadata m;
    m.hasMetadata = j.value("hasMetadata", false);
    m.generator = GeneratorFromStr(j.value("generator", std::string("Unknown")));
    m.rawText = j.value("rawText", std::string());
    m.error = j.value("error", std::string());
    m.parserRev = j.value("rev", 0);
    m.slim = j.value("slim", false);
    m.workflowMask = j.value("wfmask", 0u);
    m.isVideo = j.value("video", false);
    m.durationSec = j.value("duration", 0.0);
    m.videoW = j.value("vw", 0);
    m.videoH = j.value("vh", 0);

    if (j.contains("rawFields") && j["rawFields"].is_array()) {
        for (auto& rf : j["rawFields"]) {
            RawField f;
            f.key = rf.value("key", std::string());
            f.value = rf.value("value", std::string());
            m.rawFields.push_back(std::move(f));
        }
    }

    m.parsed.positivePrompt = j.value("positivePrompt", std::string());
    m.parsed.negativePrompt = j.value("negativePrompt", std::string());
    if (j.contains("tags") && j["tags"].is_array()) {
        for (auto& t : j["tags"])
            if (t.is_string()) m.parsed.tags.push_back(t.get<std::string>());
    }
    if (j.contains("negativeTags") && j["negativeTags"].is_array()) {
        for (auto& t : j["negativeTags"])
            if (t.is_string()) m.parsed.negativeTags.push_back(t.get<std::string>());
    }
    if (j.contains("params") && j["params"].is_array()) {
        for (auto& p : j["params"]) {
            m.parsed.params.push_back({p.value("key", std::string()), p.value("value", std::string())});
        }
    }
    // (read before 2026-10-28: a ConditioningZeroOut negative - Krea 2, Flux - came out as the positive again)
    if (!m.parsed.negativePrompt.empty() && m.parsed.negativePrompt == m.parsed.positivePrompt) {
        m.parsed.negativePrompt.clear();
        m.parsed.negativeTags.clear();
    }
    return m;
}

std::wstring ResolveCacheDir() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = (len > 0 && len < MAX_PATH) ? buf : L".";
    return base + L"\\Try2Create\\cache";
}

std::wstring HashToHex(const std::wstring& key) {
    size_t h = std::hash<std::wstring>{}(key);
    std::wostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill(L'0') << h;
    return oss.str();
}

} // namespace

void DiskCache::Load() {
    cacheDir_ = ResolveCacheDir();
    thumbsDir_ = cacheDir_ + L"\\thumbs";
    std::error_code ec;
    fs::create_directories(thumbsDir_, ec);
    ready_ = true;

    std::wstring indexPath = cacheDir_ + L"\\index.json";
    std::ifstream f(indexPath, std::ios::binary);
    if (!f) return; // no cache yet; that's fine

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (content.empty()) return;

    try {
        json arr = json::parse(content);
        if (!arr.is_array()) return;
        for (auto& item : arr) {
            std::string keyUtf8 = item.value("key", std::string());
            if (keyUtf8.empty()) continue;
            std::wstring key = Utf8ToWideLocal(keyUtf8);

            Record rec;
            rec.size = item.value("size", static_cast<uintmax_t>(0));
            long long ticks = item.value("mtimeTicks", static_cast<long long>(0));
            rec.mtime = fs::file_time_type(fs::file_time_type::duration(ticks));
            rec.thumbW = item.value("thumbW", 0);
            rec.thumbH = item.value("thumbH", 0);
            rec.thumbFileName = Utf8ToWideLocal(item.value("thumbFile", std::string()));
            rec.searchTextLower = item.value("searchText", std::string());
            rec.lastAccessTick = item.value("lastAccess", static_cast<uint64_t>(0));
            if (item.contains("meta")) rec.metadata = MetadataFromJson(item["meta"]);

            accessCounter_ = std::max(accessCounter_, rec.lastAccessTick);
            records_[key] = std::move(rec);
        }
    } catch (...) {
        // Corrupt or foreign-format index: ignore it and start fresh rather
        // than failing to launch.
    }
}

void DiskCache::Save() {
    if (!ready_) return;
    json arr = json::array();
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (records_.size() > kMaxEntries) {
            // Evict the least-recently-used entries down to 90% of the cap so
            // we're not rewriting+re-evicting on every single save right at
            // the boundary.
            std::vector<std::pair<uint64_t, std::wstring>> byAge;
            byAge.reserve(records_.size());
            for (auto& [key, rec] : records_) byAge.emplace_back(rec.lastAccessTick, key);
            std::sort(byAge.begin(), byAge.end());

            size_t target = static_cast<size_t>(kMaxEntries * 0.9);
            size_t toEvict = records_.size() - target;
            for (size_t i = 0; i < toEvict && i < byAge.size(); ++i) {
                auto it = records_.find(byAge[i].second);
                if (it == records_.end()) continue;
                std::error_code ec;
                fs::remove(thumbsDir_ + L"\\" + it->second.thumbFileName, ec);
                records_.erase(it);
            }
        }

        for (auto& [key, rec] : records_) {
            json item;
            item["key"] = WideToUtf8Local(key);
            item["size"] = rec.size;
            item["mtimeTicks"] = static_cast<long long>(rec.mtime.time_since_epoch().count());
            item["thumbW"] = rec.thumbW;
            item["thumbH"] = rec.thumbH;
            item["thumbFile"] = WideToUtf8Local(rec.thumbFileName);
            item["searchText"] = rec.searchTextLower;
            item["lastAccess"] = rec.lastAccessTick;
            item["meta"] = MetadataToJson(rec.metadata);
            arr.push_back(std::move(item));
        }
    }

    std::wstring indexPath = cacheDir_ + L"\\index.json";
    std::wstring tmpPath = indexPath + L".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) return;
        // PNG tEXt/EXIF text isn't guaranteed to be valid UTF-8 (garbled files,
        // Latin-1-per-spec-but-not-in-practice tools, etc.); dump() throws on
        // invalid bytes by default, so tell it to replace them instead of
        // crashing the loader.
        out << arr.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    std::error_code ec;
    fs::rename(tmpPath, indexPath, ec); // atomic-ish replace; leaves .tmp behind only on failure
}

bool DiskCache::TryGet(const std::wstring& lowerKey, Record& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(lowerKey);
    if (it == records_.end()) return false;
    it->second.lastAccessTick = ++accessCounter_;
    out = it->second;
    return true;
}

void DiskCache::Rekey(const std::wstring& fromKey, const std::wstring& toKey) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(fromKey);
    if (it == records_.end()) return;
    Record rec = std::move(it->second);
    records_.erase(it);
    records_[toKey] = std::move(rec);
}

size_t DiskCache::EntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

void DiskCache::Put(const std::wstring& lowerKey, fs::file_time_type mtime, uintmax_t size,
                     const ImageMetadata& metadata, const std::string& searchTextLower, const unsigned char* rgba,
                     int w, int h) {
    if (!ready_) return;
    std::wstring thumbFileName = HashToHex(lowerKey) + L".png";
    WriteThumbnailPng(thumbsDir_ + L"\\" + thumbFileName, rgba, w, h);

    Record rec;
    rec.mtime = mtime;
    rec.size = size;
    rec.thumbW = w;
    rec.thumbH = h;
    rec.metadata = metadata;
    rec.searchTextLower = searchTextLower;
    rec.thumbFileName = thumbFileName;

    std::lock_guard<std::mutex> lock(mutex_);
    rec.lastAccessTick = ++accessCounter_;
    records_[lowerKey] = std::move(rec);
}

bool DiskCache::ReadThumbBytes(const Record& record, std::vector<unsigned char>& outBytes) const {
    std::ifstream f(thumbsDir_ + L"\\" + record.thumbFileName, std::ios::binary);
    if (!f) return false;
    outBytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !outBytes.empty();
}
