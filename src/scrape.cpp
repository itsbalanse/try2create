#include "scrape.h"
#include "metadata.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <Windows.h>
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

bool IsSupportedImage(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp";
}

const char* GeneratorName(Generator g) {
    switch (g) {
        case Generator::Automatic1111: return "AUTOMATIC1111";
        case Generator::ComfyUI: return "ComfyUI";
        case Generator::NovelAI: return "NovelAI";
        case Generator::InvokeAI: return "InvokeAI";
        case Generator::Generic: return "Generic";
        default: return "Unknown";
    }
}

std::string CsvEscape(const std::string& s) {
    bool needsQuotes = s.find_first_of(",\"\n\r") != std::string::npos;
    if (!needsQuotes) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

std::string JoinTags(const std::vector<std::string>& tags, const char* sep) {
    std::string out;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) out += sep;
        out += tags[i];
    }
    return out;
}

std::string FindParam(const KeyValueList& params, const char* key) {
    for (auto& kv : params) {
        if (_stricmp(kv.first.c_str(), key) == 0) return kv.second;
    }
    return "";
}

std::string WStringToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

ScrapeResult WriteScrapeOutput(const std::vector<ImageMetadata>& metas, ScrapeFormat format,
                                const std::wstring& outputFile) {
    ScrapeResult result;

    std::ofstream out(outputFile, std::ios::binary);
    if (!out) {
        result.message = "Could not create output file";
        return result;
    }

    if (format == ScrapeFormat::Csv) {
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        out.write(reinterpret_cast<const char*>(bom), 3);
        out << "File,Generator,PositivePrompt,NegativePrompt,Tags,Steps,Sampler,CFGScale,Seed,Size,Model\n";
        for (auto& m : metas) {
            fs::path p(m.filePath);
            out << CsvEscape(WStringToUtf8(p.filename().wstring())) << ","
                << CsvEscape(GeneratorName(m.generator)) << ","
                << CsvEscape(m.parsed.positivePrompt) << ","
                << CsvEscape(m.parsed.negativePrompt) << ","
                << CsvEscape(JoinTags(m.parsed.tags, "; ")) << ","
                << CsvEscape(FindParam(m.parsed.params, "Steps")) << ","
                << CsvEscape(FindParam(m.parsed.params, "Sampler")) << ","
                << CsvEscape(FindParam(m.parsed.params, "CFG scale")) << ","
                << CsvEscape(FindParam(m.parsed.params, "Seed")) << ","
                << CsvEscape(FindParam(m.parsed.params, "Size")) << ","
                << CsvEscape(FindParam(m.parsed.params, "Model")) << "\n";
        }
    } else {
        json arr = json::array();
        for (auto& m : metas) {
            fs::path p(m.filePath);
            json entry;
            entry["file"] = WStringToUtf8(p.filename().wstring());
            entry["generator"] = GeneratorName(m.generator);
            entry["hasMetadata"] = m.hasMetadata;
            entry["positivePrompt"] = m.parsed.positivePrompt;
            entry["negativePrompt"] = m.parsed.negativePrompt;
            entry["tags"] = m.parsed.tags;
            entry["negativeTags"] = m.parsed.negativeTags;
            json params = json::object();
            for (auto& kv : m.parsed.params) params[kv.first] = kv.second;
            entry["params"] = params;
            json raw = json::object();
            for (auto& f : m.rawFields) raw[f.key] = f.value;
            entry["raw"] = raw;
            arr.push_back(std::move(entry));
        }
        // Metadata text isn't guaranteed valid UTF-8 (garbled files, tools that
        // write Latin-1 per the PNG spec, etc.); dump() throws on invalid bytes
        // by default, so replace them instead of aborting the whole export.
        out << arr.dump(2, ' ', false, json::error_handler_t::replace);
    }

    result.success = true;
    result.message = "Wrote " + std::to_string(metas.size()) + " entries";
    return result;
}

} // namespace

ScrapeResult ScrapeFolder(const std::wstring& folder, bool recursive, ScrapeFormat format,
                          const std::wstring& outputFile, ScrapeProgress* progress) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (recursive) {
        for (auto& de : fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (de.is_regular_file() && IsSupportedImage(de.path())) files.push_back(de.path());
        }
    } else {
        for (auto& de : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (de.is_regular_file() && IsSupportedImage(de.path())) files.push_back(de.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::vector<std::wstring> paths;
    paths.reserve(files.size());
    for (auto& f : files) paths.push_back(f.wstring());

    return ScrapeFiles(paths, format, outputFile, progress);
}

ScrapeResult ScrapeFiles(const std::vector<std::wstring>& files, ScrapeFormat format, const std::wstring& outputFile,
                         ScrapeProgress* progress) {
    if (progress) progress->total = static_cast<int>(files.size());

    std::vector<ImageMetadata> metas;
    metas.reserve(files.size());
    for (auto& f : files) {
        if (progress && progress->cancelRequested.load()) {
            ScrapeResult result;
            result.cancelled = true;
            result.filesScanned = static_cast<int>(metas.size());
            for (auto& m : metas) if (m.hasMetadata) result.filesWithMetadata++;
            result.message = "Cancelled";
            return result;
        }
        ImageMetadata m = ReadImageMetadata(f);
        metas.push_back(std::move(m));
        if (progress) progress->current = static_cast<int>(metas.size());
    }

    ScrapeResult result = WriteScrapeOutput(metas, format, outputFile);
    result.filesScanned = static_cast<int>(metas.size());
    for (auto& m : metas) if (m.hasMetadata) result.filesWithMetadata++;
    return result;
}
