#pragma once
// Star in a video's swap, its mask check and its object edits: Openjutsu's ComfyUI workflows (tools\openjutsu, MIT), trimmed per job
// (masked_swap.cpp, as Openjutsu's lib/simple.ts, advanced.ts and object.ts do). "@clip" is the uploaded clip, "@pic1".."@pic4" the pictures.
#include <string>
#include <vector>

#include "json.hpp"

struct MaskedSwapOptions {
    int pictures = 1;               // 1-4 people going in
    std::vector<std::string> roles; // whom each replaces, in words ("the dancer - white polo"; the part after " - " only helps find them); "" = by screen time
    std::string resolution = "736p"; // 480p 576p 736p 768p 1080p (the short side; the clip keeps its shape)
    int steps = 6;
    bool speedLora = true;           // DMAD; off: the model alone (use 20 steps or more)
    double start = 0, seconds = 0;   // the part of the clip (seconds 0 = to its end)
    double clipSeconds = 6;          // Context Loop's clips (one clip when it is the part's whole length)
    bool hatFix = false, noLipSync = false;
    int margin = 1;                  // 0 tight, 1 normal, 2 loose
    int motion = 512;                // the grey mannequins' long side: 512, 768 or 1376
    long long seed = 42;
    std::string target = "person";   // what SAM tracks (one picture, no role: the biggest one is replaced)
    std::string unet, textEncoder, speed; // this PC's files
    std::string veda;                // Veda's predictor ("" = full attention)
    bool onePass = false;            // two people: one H3 pass each, with only their picture (advanced_maxq; about twice the time)
    bool shotCheck = false;          // our Try2CreateShotList between Qwen's shot list and H3 (InstallShotCheck: loaded in ComfyUI)
    std::string outName = "H3_Swap"; // the final video, in output\video (numbered, never overwritten)
};

struct ObjectEditOptions {
    std::string mode = "change"; // change, remove, background
    std::string target;          // what SAM tracks ("hair", "jacket"; several with commas); for background, what stays ("person")
    std::string changeTo;        // what it becomes; for remove, what shows instead (or "")
    bool picture = false;        // a picture of what it becomes ("@pic1")
    bool biggestOnly = false, keepOutline = false;
    std::string resolution = "736p";
    int steps = 6;
    bool speedLora = true;
    double start = 0, seconds = 0;
    double clipSeconds = 6;
    int margin = 1; // 0 tight (10 px), 1 normal (20), 2 loose (40)
    long long seed = 42;
    std::string unet, textEncoder, speed;
    std::string veda;
    std::string outName = "H3_Edit";
};

std::wstring OpenjutsuDir();                            // tools\openjutsu next to the exe
bool InstallShotCheck();                                // tools\openjutsu\nodes\try2create_shots into ComfyUI's custom_nodes when missing or older (loads at ComfyUI's next start)
bool MaskedSwapNodePacks(std::string* missing);         // its node packs in ComfyUI's custom_nodes (what is missing, in words)
nlohmann::json BuildMaskedSwap(const MaskedSwapOptions& o, std::string* error);
nlohmann::json BuildMaskCheck(const MaskedSwapOptions& o, std::string* error); // SAM and the mask only: the area redrawn (cyan), the person (orange), the mannequin
nlohmann::json BuildObjectEdit(const ObjectEditOptions& o, std::string* error);
