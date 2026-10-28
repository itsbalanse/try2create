#pragma once
// Cloud services, all OFF until turned on in Settings > Cloud and API keys (this PC stays the way things are made):
//   - the prompt writer on OpenCode: OpenCode Go (your subscription: no cost per call) or OpenCode Zen (pay per use, priced from models.dev)
//   - pictures and videos on fal.ai: one key for the top models (Kling, Seedance, MiniMax H3 Max, Wan, Veo / Gemini, GPT Image, Nano Banana,
//     FLUX, Seedream...), with live prices from fal's own pricing API
//   - API keys kept encrypted with Windows (DPAPI: only this Windows user on this PC can read them), never written out in plain text
//   - guard rails: before anything goes to a cloud service, what might get it flagged (NSFW, real people, copyrighted characters and brands,
//     going past your monthly limit) is said, and you choose: send anyway, don't send, or don't warn about that again
//   - what it cost: every paid call is recorded (Settings shows this month's total)
#include <functional>
#include <string>
#include <vector>

struct CloudSettings {
    int writer = 0;                     // the prompt writer: 0 this PC, 1 OpenCode Go (your plan), 2 OpenCode Zen (pay per use)
    std::string writerModel = "kimi-k2.6";
    std::string visionModel = "kimi-k2.6"; // when pictures go with the request
    bool media = false;                 // cloud pictures and videos (fal.ai) offered on the pages
    std::string imageModel = "fal-ai/nano-banana-2";          // text to picture
    std::string editModel = "fal-ai/nano-banana-2/edit";      // pictures (references) to picture
    std::string videoModel = "fal-ai/kling-video/v3/standard/image-to-video"; // picture to video
    std::string textVideoModel = "fal-ai/kling-video/v3/standard/text-to-video";
    bool warnNsfw = true, warnPeople = true, warnCopyright = true;
    double monthLimit = 0;              // USD a month; a job that would pass it is asked about (0 = no limit)
};
CloudSettings& Cloud();
void SaveCloud();

// ---- keys ("opencode", "fal"), encrypted with DPAPI in %LOCALAPPDATA%\Try2Create\keys.json
bool CloudSetKey(const std::string& service, const std::string& key);
std::string CloudKey(const std::string& service);     // "" = none
std::string CloudKeyHint(const std::string& service); // "....a1b2" ("" = none)
void CloudForgetKey(const std::string& service);

// ---- the prompt writer on OpenCode
bool CloudWriterActive();        // turned on, with a key
bool CloudWriterCanSee();        // its picture model takes pictures
std::string CloudWriterLabel();  // "OpenCode Go: kimi-k2.6"
// One chat request, as WriterChat (blocks: worker threads). The guard rails ask first when the text might get flagged.
bool CloudChat(const std::string& system, const std::string& user, const std::vector<std::string>& imageDataUrls, int maxTokens, double temperature,
               std::string& out, std::string* error);
struct LlmModel {
    std::string id, name;
    bool vision = false;
    double in = -1, out = -1; // USD per million tokens (-1 = not known)
};
std::vector<LlmModel> CloudLlmModels(int plan); // 1 Go, 2 Zen: what the plan offers (empty until fetched)
void CloudRefreshLlmModels();                   // fetched in the background (the plans' model lists, prices from models.dev)
bool CloudLlmRefreshing();
std::string CloudTestWriter();                  // a tiny request: "" = it works, else why not (blocks)

// ---- pictures and videos on fal.ai
struct MediaModel {
    std::string id, name, category; // "fal-ai/kling-video/v3/standard/image-to-video", "Kling Video v3 Image to Video [Standard]", "image-to-video"
    double price = -1;              // USD per unit (-1 = not known yet)
    std::string unit;               // "second", "video", "image", "megapixel"...
};
// what fal offers in a category ("text-to-image", "image-to-image", "image-to-video", "text-to-video"), most popular first
std::vector<MediaModel> CloudMediaModels(const std::string& category);
void CloudRefreshMedia();                          // the lists and (with a key) their prices, in the background
bool CloudMediaRefreshing();
const MediaModel* CloudMediaModel(const std::string& id);
// what a job would cost: USD (-1 = not known), and how it was worked out ("$0.084 a second x 5 s")
double CloudEstimate(const std::string& model, bool video, double seconds, int width, int height, int count, std::string* how);
std::string CloudMoney(double usd);                // "$0.42", "under 1 cent", "price not known"

struct CloudMediaJob {
    std::string model;               // fal endpoint id
    std::string prompt;
    std::vector<std::wstring> images; // the first frame (video) / references (edit)
    std::wstring endImage;           // a video's last frame (optional)
    bool video = false;
    double seconds = 5;
    int width = 0, height = 0;       // what is wanted (the model picks its nearest size / shape)
    std::wstring outDir;             // where the result is saved
    std::string savePrefix = "Cloud";
};
// Runs a job on fal.ai (blocks: a worker thread): uploads, queues, waits, downloads. The guard rails ask first when it might get flagged.
// `progress` gets what is happening ("In fal's queue (2 ahead)"); `stop` is checked while waiting. Returns the saved file, or "" with `error`.
std::wstring CloudRunMedia(const CloudMediaJob& job, const std::function<void(const std::string&)>& progress, const std::function<bool()>& stop,
                           std::string* error, double* cost);

// ---- guard rails
struct CloudConcern {
    std::string kind;    // "nsfw", "people", "copyright", "budget"
    std::string message; // "\"Spider-Man\" is a copyrighted character"
};
std::vector<CloudConcern> CloudTextConcerns(const std::string& text);
// From a worker thread: shows the warning and waits for the answer (true = send). The UI draws it with DrawCloudGuard.
bool CloudGuardWait(const std::string& what, const std::vector<CloudConcern>& concerns);

// ---- what it cost
void CloudRecordCost(const std::string& what, double usd);
double CloudSpentThisMonth();
struct CloudCost {
    long long when = 0;
    std::string what;
    double usd = 0;
};
std::vector<CloudCost> CloudRecentCosts(int n);
