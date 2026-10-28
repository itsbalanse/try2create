#pragma once
#include <memory>
#include <string>

// Counts the people in an image with an ONNX object-detection model (YOLOv8
// style output, one "person" class) run through ONNX Runtime: DirectML on the
// GPU when available, CPU otherwise. Same runtime-loading approach as
// NsfwClassifier, so the rest of the app still works without the DLLs or model.
//
// Model lookup: <exe dir>\models\people.onnx (or ..\..\models\ for a build/bin
// exe). Optional people.json beside it:
//   { "input_size": 640, "threshold": 0.324, "nms_iou": 0.5, "person_class": 0 }
class PersonDetector {
public:
    PersonDetector();
    ~PersonDetector();
    PersonDetector(const PersonDetector&) = delete;
    PersonDetector& operator=(const PersonDetector&) = delete;

    // Loads the runtime and model on first call (seconds); later calls return
    // the cached outcome. Thread-safe.
    bool EnsureLoaded(std::string* error);
    bool IsLoaded() const;
    std::string DeviceName() const; // "DirectML (GPU)" or "CPU" once loaded

    // Identifies the model file (name + size) without loading it, so cached
    // counts from a different model can be discarded. Empty if there is none.
    static std::string ModelId();

    // Number of people found (0 or more), or -1 if the image couldn't be
    // decoded or inference failed. Safe to call from several threads: decoding
    // and preprocessing run in parallel, inference is serialized.
    int CountPeopleFile(const std::wstring& path, std::string* error);
    int CountPeopleRGBA(const unsigned char* rgba, int width, int height, std::string* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
