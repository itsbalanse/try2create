#pragma once
// Picture quality ("does it look good") with two small models, run through ONNX Runtime (DirectML on the GPU when
// available, CPU otherwise):
//   - a general aesthetic predictor (CLIP ViT-L/14 image embedding + the LAION aesthetic head), good for photos / realistic art
//   - an anime aesthetic model (skytnt/anime-aesthetic), good for illustrations
// The CLIP embedding also tells which kind of picture it is (anime vs photo), so the right model's opinion is the one shown.
//
// Files (next to the exe, in models\):  aesthetic_clip.onnx, aesthetic_anime.onnx, aesthetic.json
#include <memory>
#include <string>
#include <vector>

// What kind of picture it is (zero-shot from the CLIP embedding). Same order as "kind_vecs" in aesthetic.json.
inline constexpr int kStyleCount = 7;
inline constexpr const char* kStyleNames[kStyleCount] = {"Realistic / photo", "Anime / manga", "Cartoon", "3D render",
                                                         "Painting / digital art", "Pixel art", "Sketch / line art"};

class AestheticScorer {
public:
    struct Scores {
        float anime = -1;     // anime model, 0..10 (negative = unknown)
        float general = -1;   // general model, 0..10 (the LAION score as it is: about 5.5 is average, 7+ is very good)
        float animeProb = -1; // 0..1: how much it looks like anime / illustration rather than a photo
        int style = -1;       // index into kStyleNames (negative = unknown)
        float styleConf = -1; // 0..1: how sure the style is
        // The one number shown: the anime model's score for anime pictures, the general one for the rest.
        float Quality() const {
            if (anime < 0 || general < 0 || animeProb < 0) return general >= 0 ? general : anime;
            return animeProb * anime + (1.0f - animeProb) * general;
        }
    };

    AestheticScorer();
    ~AestheticScorer();
    AestheticScorer(const AestheticScorer&) = delete;
    AestheticScorer& operator=(const AestheticScorer&) = delete;

    static bool ModelsPresent();     // are the three files there
    static std::string ModelId();    // name+sizes, so cached scores from other models are dropped
    bool EnsureLoaded(std::string* error);
    bool IsLoaded() const;
    std::string DeviceName() const;

    // Safe from several threads: decoding / resizing run in parallel, inference is serialized.
    bool ScoreFile(const std::wstring& path, Scores& out, std::string* error);
    bool ScoreRGBA(const unsigned char* rgba, int width, int height, Scores& out, std::string* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
