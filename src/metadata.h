#pragma once
#include <string>
#include <vector>
#include <utility>

// Generation tool detected from embedded metadata.
enum class Generator {
    Unknown,
    Automatic1111,
    ComfyUI,
    NovelAI,
    InvokeAI,
    Generic
};

using KeyValueList = std::vector<std::pair<std::string, std::string>>;

// A single raw text/metadata field as found in the file (PNG tEXt/iTXt/zTXt
// keyword+value, or an EXIF/XMP field), in the order encountered.
struct RawField {
    std::string key;
    std::string value;
};

struct ParsedFields {
    std::string positivePrompt;
    std::string negativePrompt;
    std::vector<std::string> tags;          // split from positivePrompt
    std::vector<std::string> negativeTags;  // split from negativePrompt
    KeyValueList params;                    // Steps, Sampler, CFG scale, Seed, Model, ...
};

struct ImageMetadata {
    std::wstring filePath;
    bool hasMetadata = false;
    Generator generator = Generator::Unknown;
    std::vector<RawField> rawFields; // everything found, in file order
    std::string rawText;             // human-readable concatenation for display/copy
    ParsedFields parsed;
    std::string error; // non-empty if the file could not be read/parsed at all
    bool slim = false;        // big raw fields (ComfyUI workflows) were left out of this copy; read the file for them
    unsigned workflowMask = 0; // bit per WorkflowKind present in the file (set even when slim)
    int parserRev = 0;       // which version of the readers produced this (cached results from older ones are re-read)
    bool isVideo = false;    // a video clip (poster frame, duration; the workflow is read from its metadata)
    double durationSec = 0;
    int videoW = 0, videoH = 0;
};

// Bumped when the readers learn something new (ComfyUI subgraphs/video prompts, WebP workflows ...).
constexpr int kParserRevision = 3;

// What the gallery keeps per image (and the on-disk cache stores): everything except huge raw fields such as
// whole ComfyUI workflows, which would make the cache hundreds of MB. Selecting an image reads the full data again.
void SlimMetadata(ImageMetadata& meta);

// Reads embedded generation metadata from a PNG/JPEG/WEBP file at `path`.
// Never throws; failures are reported via ImageMetadata::error.
ImageMetadata ReadImageMetadata(const std::wstring& path);

// Splits a prompt string into individual tags on top-level commas
// (ignoring commas nested inside (), [] or {} weighting groups).
std::vector<std::string> SplitPromptTags(const std::string& prompt);
