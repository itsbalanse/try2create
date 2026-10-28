#pragma once
// Clef (Cloudflare's decision model, https://huggingface.co/Cloudflare/clef-flash): it does not write text, it answers typed questions with a probability for every
// option, about a text and/or pictures. The app uses it to choose (outfits, poses, scenarios), to judge ideas, to grade pictures (NSFW level, aesthetic quality, style),
// to tag the Vision library, to rate prompts and to check faces. It runs as a local llama.cpp server (llama-server with the Clef GGUF and its mmproj file), which has a
// POST /v1/systemone API. Without it the choices fall back to plain random ones.
#include <functional>
#include <string>
#include <vector>

struct ClefSettings {
    bool use = true;                      // ask Clef when it is running (otherwise random choices)
    std::string url = "http://127.0.0.1:8099";
    std::wstring exe;                     // llama-server.exe (to start it from the app)
    std::wstring model;                   // Clef-Flash-*.gguf
    std::wstring mmproj;                  // mmproj-Clef-Flash-*.gguf (lets it look at pictures)
    int gpuLayers = 0;                    // 0 = CPU (keeps the GPU free for ComfyUI), 99 = everything on the GPU
    bool autoStart = false;               // start it with Clef mode when it is not running
    double temperature = 1.4;             // > 1 spreads the choices out, 1 follows Clef's probabilities exactly
    // the prompt writer (an ordinary LLM, see WriterChat)
    std::string writerUrl = "http://127.0.0.1:8100";
    std::wstring writerModel;             // e.g. Qwen3.5-9B-...-Q8_0.gguf
    std::wstring writerMmproj;            // its mmproj file (lets it look at pictures)
    int writerGpuLayers = 99;
    bool writerUseRunning = true;         // when a llama-server with the same model is already running (Unsloth Studio), use that one instead of loading another
};

ClefSettings ClefGetSettings();           // a copy (thread safe)
void ClefSetSettings(const ClefSettings& s);   // also saved to disk
void ClefFindDownloaded();                     // the first-run setup downloaded llama.cpp / the writer model: use them (fills only what is unset)

enum class ClefType { Choice = 0, Score = 1, Noul = 2 };

struct ClefQuestion {
    std::string id;
    std::string instructions;
    std::vector<std::string> options;     // choice: what each option means. score: the levels, lowest first (2 to 10). noul: unused
    ClefType type = ClefType::Choice;
};

struct ClefAnswer {
    std::vector<double> probs;            // choice / score: probability of each option / level
    double score = 0;                     // score: the expected level index (can be between two levels)
    double pTrue = 0;                     // noul: probability that the answer is yes
};

// Asks every question in one request about `stateJson` (a JSON value or plain text) and the pictures (data URLs, see ImageDataUrl), which are placed before it.
bool ClefAsk(const std::string& stateJson, const std::vector<std::string>& imageDataUrls, const std::vector<ClefQuestion>& questions, std::vector<ClefAnswer>& answers, std::string* error = nullptr);
// The same for choice questions about text only: probabilities[i][j] is the probability of options[j] of question i.
bool ClefDecide(const std::string& stateJson, const std::vector<ClefQuestion>& questions, std::vector<std::vector<double>>& probabilities, std::string* error = nullptr);
bool ClefWaitReady(int timeoutSeconds);   // waits until the server answers (false: it did not in time, or it died)
long long GpuFreeMiB();                   // free memory of the first NVIDIA GPU in MiB (nvidia-smi), -1 when unknown
void FreeOllamaModels();                  // asks Ollama (default address) to unload whatever it holds in memory
bool ClefPing();                          // is the server answering (short timeout)?
bool ClefStart(std::string& message);     // starts llama-server with the model (a window-less background process, closed with the app)
bool ClefOwned();                         // did this app start it?
void ClefStopOwned();

// Taking turns on the GPU (Qwen alone takes most of a 24 GB card). Acquire: if there is not enough free memory ComfyUI is asked to unload its models (then Ollama's),
// Clef is loaded and waited for. Release: the server is closed again when this call started it (immediate), or left to the idle timer (a few more requests may follow).
struct ClefSession {
    bool ok = false;      // Clef is answering
    bool started = false; // this call started it (so this call may stop it)
};
ClefSession ClefAcquire(const std::string& comfyUrl, const std::function<void(const std::string&)>& phase);
void ClefRelease(const ClefSession& session, bool immediate);
void ClefTouch();         // "just used": restarts the idle timer
void ClefHousekeeping();  // call often (each frame): closes a server this app started that has been idle for a while

// ---------------------------------------------------------------------------------------------------------------
// The prompt writer: an ordinary LLM served by llama-server (its own on port 8100, or yours when it is already running). It writes scene prompts from a Vision board, and
// - with its mmproj file - describes a character from the picture. It takes turns on the GPU with Clef and ComfyUI the same way Clef does.
bool WriterConfigured();      // a running server with this model was found, or the program and the model file are there
bool WriterCanSee();          // it can look at pictures
std::string WriterStatusText();
ClefSession WriterAcquire(const std::string& comfyUrl, const std::function<void(const std::string&)>& phase);
void WriterRelease(const ClefSession& session, bool immediate);
bool WriterOwned();
void WriterStopOwned();
void WriterTouch();
void WriterHousekeeping();
void LlmStopOwned();          // closes whatever this app loaded for Clef or the writer (ComfyUI needs the GPU)
// One chat request. `user` may come with pictures (data URLs, see ImageDataUrl). out = the reply without any thinking.
bool WriterChat(const std::string& system, const std::string& user, const std::vector<std::string>& imageDataUrls, int maxTokens, double temperature, std::string& out, std::string* error = nullptr);
// The same, the answer held to a JSON schema (llama-server turns it into a grammar: the form cannot be skipped; minLength keeps fields from
// coming out as two words). `out` is the JSON text. Tested 2026-10-23: Bonsai 27B and the 9B both wrote markdown and skipped the video form
// when asked in words; with a schema they keep it.
bool WriterChatJson(const std::string& system, const std::string& user, const std::vector<std::string>& imageDataUrls, const std::string& schema, int maxTokens,
                    double temperature, std::string& out, std::string* error = nullptr);

// ---------------------------------------------------------------------------------------------------------------
// The embedding model (EmbeddingGemma-2, tools\embed): text and pictures become vectors of ONE space, so a sentence can be compared with a picture. Used to search the gallery
// by meaning, to find pictures like one, and to keep Clef mode varied. A llama-server of its own (port 8101, about 3 GB), taking turns on the GPU like the others.
constexpr int kEmbedDim = 256; // the vectors are cut to this size (Matryoshka: almost no loss) and normalised: their dot product is their cosine
bool EmbedAvailable();         // the model files and llama-server are there
ClefSession EmbedAcquire(const std::string& comfyUrl, const std::function<void(const std::string&)>& phase);
void EmbedRelease(const ClefSession& session, bool immediate);
bool EmbedOwned();
void EmbedStopOwned();
void EmbedTouch();
void EmbedHousekeeping();
// Texts: for a search use the prefix "task: search result | query: ". out[i] belongs to texts[i].
bool EmbedTexts(const std::vector<std::string>& texts, std::vector<std::vector<float>>& out);
bool EmbedImageData(const std::string& dataUrl, std::vector<float>& out); // a picture as a data URL (see ImageDataUrl)
