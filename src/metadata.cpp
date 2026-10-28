#include "metadata.h"
#include "metadata_png.h"
#include "metadata_jpeg.h"
#include "metadata_parsers.h"
#include "video_util.h"
#include "workflow.h"
#include <algorithm>

namespace {

std::wstring ToLowerW(const std::wstring& s) {
    std::wstring r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::towlower);
    return r;
}

std::wstring ExtensionOf(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    return ToLowerW(path.substr(dot));
}

void BuildRawText(ImageMetadata& meta) {
    std::string text;
    for (const auto& f : meta.rawFields) {
        if (!text.empty()) text += "\n\n";
        text += "[" + f.key + "]\n" + f.value;
    }
    meta.rawText = text;
}

} // namespace

ImageMetadata ReadImageMetadata(const std::wstring& path) {
    ImageMetadata meta;
    meta.filePath = path;
    meta.parserRev = kParserRevision;

    std::wstring ext = ExtensionOf(path);
    std::string err;
    bool ok = false;

    if (ext == L".png") {
        ok = ReadPngTextChunks(path, meta.rawFields, &err);
    } else if (ext == L".jpg" || ext == L".jpeg") {
        ok = ReadJpegTextFields(path, meta.rawFields, &err);
    } else if (ext == L".webp") {
        ok = ReadWebpTextFields(path, meta.rawFields, &err);
    } else if (IsVideoPath(path)) {
        meta.rawFields = ReadVideoWorkflowFields(path);
        ok = true;
        meta.isVideo = true;
        const VideoInfo info = ProbeVideo(path);
        meta.durationSec = info.seconds;
        meta.videoW = info.width;
        meta.videoH = info.height;
    } else {
        err = "Unsupported file type";
    }

    if (!ok) {
        meta.error = err;
        return meta;
    }

    BuildRawText(meta);
    InterpretRawFields(meta);
    if (meta.isVideo) {
        // what the file itself says about the clip, next to whatever the workflow reported
        bool hasSize = false;
        for (const auto& kv : meta.parsed.params) hasSize = hasSize || kv.first == "Size";
        if (!hasSize && meta.videoW > 0) meta.parsed.params.push_back({"Size", std::to_string(meta.videoW) + "x" + std::to_string(meta.videoH)});
        if (meta.durationSec > 0) meta.parsed.params.push_back({"Duration", FormatDuration(meta.durationSec)});
    }
    for (const EmbeddedWorkflow& w : FindWorkflows(meta)) meta.workflowMask |= 1u << static_cast<int>(w.kind);
    return meta;
}

void SlimMetadata(ImageMetadata& meta) {
    constexpr size_t kLimit = 4096;
    bool trimmed = false;
    for (RawField& f : meta.rawFields) {
        if (f.value.size() > kLimit) {
            f.value.clear();
            f.value.shrink_to_fit();
            trimmed = true;
        }
    }
    if (trimmed) {
        meta.slim = true;
        meta.rawText.clear();
        meta.rawText.shrink_to_fit();
    }
}
