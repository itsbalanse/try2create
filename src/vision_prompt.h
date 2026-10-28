#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Natural-language "recreate this image" prompts from a local vision model
// served by Ollama (http://127.0.0.1:11434). Ollama loads the model into GPU
// memory on the first request and keeps it for `keep_alive`; Unload() frees
// it immediately. All calls block, so run them off the UI thread.

struct VisionModelInfo {
    std::string name;
    std::string sizeLabel;  // e.g. "25.8B Q4_K_M"
    bool supportsThinking = false;
};

struct OllamaEndpoint {
    std::wstring host = L"127.0.0.1";
    int port = 11434;
};

// True if the Ollama server answers.
bool OllamaIsRunning(const OllamaEndpoint& ep);

// Starts `ollama serve` (ollama.exe from PATH) without a window and waits up
// to ~20 s for it to answer.
bool OllamaStartServer(const OllamaEndpoint& ep, std::string* error);

// When on, an Ollama server started by this app is tied to it with a Windows job
// object: if the app exits for ANY reason (even being force-closed or crashing),
// Windows ends that server too. Servers that were already running are not touched.
void OllamaSetKillWithApp(bool enabled);

// Stops the Ollama backend entirely: the tray app (which would otherwise
// respawn the server), the server, and its model runner processes.
void OllamaStopServer();

// Installed models that report the "vision" capability.
bool OllamaListVisionModels(const OllamaEndpoint& ep, std::vector<VisionModelInfo>& out, std::string* error);

// Names of the models currently loaded in memory.
bool OllamaLoadedModels(const OllamaEndpoint& ep, std::vector<std::string>& out, std::string* error);

// Loads the model without generating anything (keepAlive e.g. "10m", "-1").
bool OllamaLoadModel(const OllamaEndpoint& ep, const std::string& model, const std::string& keepAlive,
                     std::string* error);

// Frees the model's memory right away.
bool OllamaUnloadModel(const OllamaEndpoint& ep, const std::string& model, std::string* error, int timeoutMs = 30000);

// Describes an image. `tags` (the image's original prompt, may be empty) is
// passed as a hint. Returns the prompt text, trimmed.
bool OllamaDescribeImage(const OllamaEndpoint& ep, const std::string& model, bool modelThinks,
                         const std::string& keepAlive, const std::string& instruction, const std::string& tags,
                         const std::wstring& imagePath, std::string& outText, std::string* error,
                         int numPredict = 0, double temperature = 0.4);

// The default instruction sent with each image.
const char* DefaultVisionInstruction();

// Styles an image prompt can be re-imagined in ("describe it as if it were an anime still").
struct VisionStyle {
    const char* key;       // used in the result file name: <name>.style-<key>.txt
    const char* label;
    const char* directive; // what the model is told the style is
};
int VisionStyleCount();
const VisionStyle& VisionStyleAt(int index);
// File-name friendly form of free text ("Dark Fantasy!" -> "dark-fantasy").
std::string StyleSlug(const std::string& text);
// The base instruction plus the style conversion request.
std::string BuildStyledInstruction(const std::string& baseInstruction, const std::string& styleDirective);

// Instruction for a Danbooru-style tag list (for SD / Illustrious / Pony models).
const char* DefaultTagsInstruction();

// "Describe the video this image could become": the image is the first frame
// and the model writes a detailed scene for `seconds` of footage.
struct VideoSceneOptions {
    int seconds = 10;       // 5-30
    bool timeline = true;   // time-coded beats; false = one flowing paragraph
    int camera = 0;         // index into VideoCameraLabel (0 = let the model decide)
    bool sound = true;      // add a "Sound:" line
    std::string extra;      // free-form direction from the user
    std::string templateText; // the user's own wording of the instruction; empty = DefaultVideoTemplate()
};
// The built-in video instruction with {PLACEHOLDERS} that BuildVideoInstruction fills in:
// {SECONDS} {PACE} {CAMERA} {STRUCTURE} {SOUND} {EXTRA}
const char* DefaultVideoTemplate();
std::string BuildVideoInstruction(const VideoSceneOptions& options);
int VideoCameraCount();
const char* VideoCameraLabel(int index);
int VideoBeatCount(int seconds);   // how many time-coded beats a clip of this length gets
int VideoTokenBudget(int seconds); // generation limit so long clips aren't cut off

// Copies `imagePath` into `outDir` (never moving the original) and writes the
// prompt next to it as <same name>.txt. If a file with that name is already
// there and is the same image, it's reused (its .txt is replaced); a
// different file with the same name gets "name (2).ext". Returns the copied
// image path.
bool SaveImageWithPrompt(const std::wstring& imagePath, const std::wstring& outDir, const std::string& text,
                         std::wstring& savedImagePath, std::string* error, const std::wstring& txtSuffix = L"");

// True if `imagePath` has already been described into `outDir` (its copy and
// the .txt, using the same naming as SaveImageWithPrompt) for this output kind.
bool HasSavedResult(const std::wstring& imagePath, const std::wstring& outDir, const std::wstring& txtSuffix = L"");

// Runs Ollama requests on one background thread so the UI never blocks
// (a cold model load takes a minute or two). The UI thread posts requests
// and polls GetSnapshot()/TakeResults() each frame.
class VisionService {
public:
    struct Item {
        std::wstring path;
        std::string tags; // original prompt, empty if none / not wanted
        bool readTags = false; // read the original prompt from the file when it's needed (big batches)
        std::wstring saveDir;  // where this image's result goes when it differs from the batch's folder (empty = the batch's)
    };
    struct Result {
        std::wstring path;
        std::string text;  // empty on failure
        std::string error;
        std::wstring savedPath; // set when auto-saved
        std::string mode;       // which kind of output this is ("" = image prompt, "tags", "video-10s-t", ...)
    };
    // How one Describe batch should behave beyond the instruction text.
    struct DescribeOptions {
        std::string mode;          // copied into each Result
        std::wstring txtSuffix;    // inserted before ".txt" when auto-saving, e.g. L".video-10s"
        int numPredict = 0;        // max tokens to generate (0 = model default)
        double temperature = 0.4;
    };
    struct Snapshot {
        bool checked = false; // a status refresh has completed at least once
        bool serverRunning = false;
        bool busy = false;
        std::string activity; // what the worker is doing right now
        std::vector<VisionModelInfo> models;
        std::vector<std::string> loadedModels;
        std::string lastError;
        int current = 0, total = 0;
    };

    VisionService();
    ~VisionService(); // unloads a model this app loaded, then stops the worker

    void Refresh(const OllamaEndpoint& ep);
    // Cheap: only re-reads which models are loaded (skipped while busy).
    void PollLoaded(const OllamaEndpoint& ep);
    void StartServer(const OllamaEndpoint& ep);
    void Load(const OllamaEndpoint& ep, const std::string& model, const std::string& keepAlive);
    void Unload(const OllamaEndpoint& ep, const std::string& model);
    // autoSaveDir: copy each image there with its .txt (empty = don't save).
    void Describe(const OllamaEndpoint& ep, const std::string& model, const std::string& keepAlive,
                  const std::string& instruction, std::vector<Item> items, const std::wstring& autoSaveDir,
                  const DescribeOptions& options = DescribeOptions());
    // One free-form request: text plus any number of images, answered as one piece of text (the Vision board uses it
    // to write a scene prompt from its elements, and to describe a single board image). The Result comes back through
    // TakeResults() with mode "compose" and `path` = the request's tag.
    struct ComposeRequest {
        std::string tag;
        std::string prompt;
        std::vector<std::wstring> images; // attached in this order
        int numPredict = 1000;
        double temperature = 0.7;
        int maxImageSide = 1024;
    };
    void Compose(const OllamaEndpoint& ep, const std::string& model, const std::string& keepAlive, ComposeRequest request);
    void Cancel(); // drops queued work and stops a batch after the current image
    // Mirrors the "stop Ollama when I close the app" setting (see OllamaSetKillWithApp).
    void SetStopOnExit(bool stop) { OllamaSetKillWithApp(stop); }

    // True once anything talked to Ollama this session (window opened,
    // model loaded, image described).
    bool UsedThisSession() const;

    // App-exit cleanup, synchronous: cancels work, unloads every model
    // Ollama has in memory, and with stopServer also shuts Ollama down.
    void Shutdown(bool stopServer);

    Snapshot GetSnapshot() const;
    std::vector<Result> TakeResults();

private:
    struct Shared;
    void Post(const OllamaEndpoint& ep, std::function<void()> task);
    std::shared_ptr<Shared> shared_;
};

// A picture as a data URL ("data:image/jpeg;base64,...") for a model that takes images: shrunk so the long side is at most maxSide. "" when it cannot be read.
std::string ImageDataUrl(const std::wstring& path, int maxSide);
