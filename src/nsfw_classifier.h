#pragma once
#include <memory>
#include <string>
#include <vector>

// Where an NSFW score comes from: Combined takes the higher of the image
// model score and the prompt keyword score (either flagging is enough).
enum class NsfwSource { Combined, ImageModel, Prompt };

// Scores images in [0,1] for NSFW content (0 = safe, 1 = explicit) using an
// ONNX image-classification model run through ONNX Runtime: DirectML on the
// GPU when available, CPU otherwise.
//
// onnxruntime.dll is loaded at runtime from next to the exe rather than
// linked, so the rest of the app keeps working (with the image classifier
// reported as unavailable) if the DLL or the model file is missing.
//
// Model lookup: <exe dir>\models\nsfw.onnx (or ..\..\models\ for a build/bin
// exe). An optional nsfw.json beside it can override the preprocessing for a
// different model:
//   { "input_size": 384, "mean": [0.5,0.5,0.5], "std": [0.5,0.5,0.5],
//     "nsfw_index": 1, "softmax": true }
class NsfwClassifier {
public:
    NsfwClassifier();
    ~NsfwClassifier();
    NsfwClassifier(const NsfwClassifier&) = delete;
    NsfwClassifier& operator=(const NsfwClassifier&) = delete;

    // Loads the runtime and model on first call (slow: seconds); later calls
    // return the cached outcome. Thread-safe.
    bool EnsureLoaded(std::string* error);
    bool IsLoaded() const;

    // "DirectML (GPU)" or "CPU" once loaded.
    std::string DeviceName() const;

    // Identifies the model file on disk (name + size) without loading it, so
    // cached scores from a different model can be discarded. Empty if none.
    static std::string ModelId();

    // NSFW probability for an image file, or a negative value if the image
    // couldn't be decoded or inference failed. Safe to call from several
    // threads at once: decoding/preprocessing runs in parallel, inference is
    // serialized (the DirectML provider doesn't support concurrent Run calls).
    // Decodes with stb_image, falling back to Windows Imaging Component for
    // formats stb can't read (e.g. WebP).
    float ScoreFile(const std::wstring& path, std::string* error);

    // Scores an already-decoded RGBA8 image.
    float ScoreRGBA(const unsigned char* rgba, int width, int height, std::string* error);

    // Samples a handful of frames spread across a video with ffmpeg (found on
    // PATH) and returns the highest frame score, or negative on failure.
    float ScoreVideo(const std::wstring& path, std::string* error);

    // True if ffmpeg.exe is on PATH (needed for ScoreVideo).
    static bool FfmpegAvailable();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Decodes an image file to RGBA8 with stb_image, falling back to Windows
// Imaging Component (WebP etc.). Thread-safe.
bool DecodeImageFileRGBA(const std::wstring& path, std::vector<unsigned char>& rgba, int& width, int& height);

// Keyword-based NSFW score in [0,1] from a positive prompt (e.g. "nude" or
// "rating:explicit" -> 1.0, "lingerie" -> 0.6). Cheap, works without the
// model, and is quite reliable for tag-style prompts.
float PromptNsfwScore(const std::string& positivePrompt);
