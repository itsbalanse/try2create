#pragma once
// The video tools under the Edits > Videos player (video_fx.cpp), and the two that are MiniMax H3 workflows (video_ui.cpp)
#include <string>

#include "json.hpp"

class AppState;

void DrawVideoFx(AppState& app, const std::wstring& video); // (video_fx.cpp)

// (video_ui.cpp) "@clip" is the uploaded clip
bool VideoLtxReady();                // LTX-2.5's own files (the Video page's LTX-2.5 quality)
std::string VideoH3ExtendModel();    // what Extend makes with ("" = no H3 model yet)
std::string VideoRef2vaModel();      // Reshoot's ref2va model (PRISM when it is there)
// The reference-to-video model and how it samples (Make a music video): Fast = the reference turbo LoRA, Best = the model alone
struct H3RefStack {
    std::string unet, turbo, textEncoder, videoVae, audioVae, vedaPredictor; // turbo "" = none
    int steps = 4;
    std::string sampler = "res_multistep", scheduler = "simple";
    double shiftVideo = 0, shiftAudio = 0; // 0 = the model's own (no shift node)
    bool veda = false;
    std::string sigmas;   // its own schedule ("" = the scheduler)
    std::string about;    // in words, for the page ("PRISM + the 4-step turbo, 4 steps, Veda")
};
// Make a music video's models: 0 Fast, 1 Best, 2 Steady, 3 Omni, 4 Pika (H3 with reference pictures), 5 LTX-2.5 (BuildLtxMusic)
H3RefStack VideoRefStack(int model);
bool VideoRefHas(int model);
bool VideoVedaOn();           // the Video page's Veda switch
void SetVideoVeda(bool on);
bool VideoVedaCan();          // its files are in ComfyUI
struct LtxMusicClip {
    std::string prompt;
    double start = 0; // in the part of the song (seconds)
    int frames = 361; // 8n + 1 at 24 fps
};
nlohmann::json BuildLtxMusic(const std::vector<LtxMusicClip>& clips, int w, int h, unsigned seed, const std::string& prefix);
nlohmann::json BuildExtendVideo(const std::string& prompt, double seconds, int clipW, int clipH);
nlohmann::json BuildReshootVideo(const std::string& prompt, double seconds, double azimuth, double elevation, double distance, bool keepSound,
                                 const std::string& crossViewLora, const std::string& turboLora);
