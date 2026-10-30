#pragma once
// Sending prompts to a running ComfyUI (its HTTP API) so it generates pictures and videos from them: the workflow (the one ComfyUI ran last,
// one saved inside ComfyUI, or a file) is loaded, the prompt is written into its text node(s), the seed is changed, and it is queued with
// POST /prompt. Every send is tracked from "waiting" to "done" so the app can show exactly what is happening.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// A LoRA to load on top of a model: a file of ComfyUI's models/loras, how strongly, and whether it is on.
struct LoraEntry {
    std::string name;
    double strength = 1.0;
    bool on = true;
};

// A distilled "turbo" model (4-8 steps, cfg 1) rather than a full one (25+ steps)?
inline bool IsTurboModel(const std::string& file) {
    std::string low;
    for (char c : file) low += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return low.find("turbo") != std::string::npos || low.find("lightning") != std::string::npos;
}

struct ComfySettings {
    std::string url = "http://127.0.0.1:8188";
    bool useLastRun = true;         // run the workflow ComfyUI ran last (or is running now): no file to export or choose
    std::string savedWorkflow;      // or one of the workflows saved inside ComfyUI (path as ComfyUI lists it, "" = none)
    bool autoImport = true;         // copy ComfyUI's saved workflows into the Workflows tab by themselves
    std::wstring workflowPath;      // otherwise: an API-format .json, a ComfyUI workflow file or a picture ComfyUI made
    bool randomSeed = true;         // a new random seed for every run
    bool sendNegative = false;      // also use the picture's negative prompt (gallery pictures)
    bool sendPicture = false;       // upload the picture and put it in the workflow's LoadImage node
    int count = 1;                  // pictures per click (each with its own seed)
    bool videoLength = true;        // video workflows: set the clip length from the prompt's seconds (frames = seconds x fps)
    int defaultFps = 24;            // used when the workflow has no fps of its own
    bool autoStart = true;          // ComfyUI is not running: start it and send when it is up
    int startMode = 0;              // 0 = a background server (no window), 1 = the ComfyUI Desktop app
    bool closeWithApp = true;       // ComfyUI that this app started is closed with this app
    bool background = true;         // the server it starts runs behind other programs: CPU and GPU priority below normal (the desktop, a browser, this app stay smooth)
    double vramHeadroom = 3.0;      // GB of graphics memory the background server keeps free (ComfyUI 0.40+ --vram-headroom; 0 = ComfyUI's own default)
    double ramHeadroom = 40.0;      // GB of RAM it keeps free: below that it lets go of cached results and pinned model copies (--cache-ram; 0 = its default, 10 GB)
    bool showWindow = false;        // start mode 1: show the Desktop window
    std::wstring desktopExe;        // ComfyUI Desktop's program ("" = look for it)
    std::wstring outputDir;         // where ComfyUI saves what it makes ("" = look it up in ComfyUI Desktop's settings)
    std::wstring comfyFolder;       // a ComfyUI that is not ComfyUI Desktop (the portable zip, a git clone): its folder ("" = ComfyUI Desktop's)
    int editStyle = 1;              // picture editing with characters: 0 = as asked, 1 = "nano banana" style (loose, natural, free to re-pose), 2 = fit everything to the scene (keeps face, body and character style)
    bool directOutfit = true;       // board scenes: name a concrete new outfit for every character (the model only changes clothes when told what to change)
    bool directPose = true;         // ...a concrete body posture
    bool directExpression = true;   // ...a concrete facial expression
    bool editJump = false;          // go to the Edits tab each time an edit is sent
    std::string editUnet; // the Qwen model picture editing loads (blank, new settings: Comfy-Org's, the turbo first) (a file in models/diffusion_models: .safetensors or .gguf; blank = find the standard Qwen Image 2.1 one)
    std::vector<LoraEntry> editLoras;    // LoRAs of the built-in picture editing (Qwen), in this order
    std::vector<LoraEntry> promptLoras;  // LoRAs added to the ComfyUI workflow chosen in the ComfyUI tab (Krea or any other): put on the model that feeds its sampler(s)
    bool promptReplaceLoras = false;     // ...after taking out the LoRA loaders that workflow already has
    std::vector<LoraEntry> upscaleLoras; // LoRAs of the MOSS redraw pass of the Upscale tab
    // LoRAs of the app's own makers, one list per model (Generate and Create a character share them): Krea 2, Qwen Image 2.1 (text to
    // image), MOSS-Image and MiniMax H3 (videos from the Generate page)
    std::vector<LoraEntry> kreaLoras, qwenLoras, mossLoras, h3Loras;
    bool loraTriggers = true;       // their trigger words (from the file or Civitai) put into the prompt when they are on
    int editSteps = 25;             // picture editing (Qwen Image 2.1 Edit, built in): sampler steps
    int editResolution = 1024;      // ...and the size budget in pixels (0 = the size of the first picture)
    std::string editEngine = "flux3"; // "flux3" (FLUX 3 Image Klein: faster, follows the words more closely; the default since 2026-10-29) or "qwen" (Qwen Image 2.1 Edit)
    std::wstring pixelModels;       // the folder of the Retro Diffusion models (pixel art) ("" = the "retrodiffusion" folder next to this program)
    std::string promptNode;         // node id of the prompt text when the automatic search picks the wrong one ("" = automatic)
    std::string negativeNode;
};

// A LoRA's file name without its folder, lower case: ComfyUI lists a LoRA in a subfolder as "Krea 2\name.safetensors", and the app (and
// workflows saved before the LoRAs were sorted) may name it without the folder. Listed: in the list under either name.
std::string LoraFileName(const std::string& name);
bool LoraListed(const std::vector<std::string>& list, const std::string& name);
// The LoRA files ComfyUI offers (from its LoraLoaderModelOnly node); empty when it cannot be reached. Blocks briefly: run it on a worker thread.
std::vector<std::string> ComfyListLoras(const std::string& url);
// The choices of one node input, e.g. ("UNETLoader", "unet_name") or ("CLIPLoader", "type"); empty when unreachable. Blocks briefly.
std::vector<std::string> ComfyListChoices(const std::string& url, const std::string& node, const std::string& input);
// The checkpoint files ComfyUI offers (CheckpointLoaderSimple), read fresh; false when it cannot be reached. Blocks briefly.
bool ComfyListCheckpoints(const std::string& url, std::vector<std::string>& out);
// Veda sparse attention (a custom node that speeds up MiniMax H3): is its node there, and which predictors are in models/veda. Blocks briefly.
struct ComfyVeda {
    bool reachable = false;              // ComfyUI answered
    bool node = false;                   // the VedaSparseAttention node is installed
    std::vector<std::string> predictors; // the files in models/veda
    bool fizgig = false;                 // the FizgigH3Tweaks node (Fizgig H3 Tweaks: detail & contrast, prompt strength) is installed
    bool continuum = false;              // H3 Continuum's sampler (H3ContinuumSamplerV39: videos longer than 15 s, in parts that continue each other)
    std::string sam3;                    // a SAM 3 checkpoint ComfyUI has (it finds the person in every frame), "" = none
};
ComfyVeda ComfyAskVeda(const std::string& url);
// Where the generation is, from the progress bar ComfyUI writes to the log of a ComfyUI this app started ("75%|###| 3/4 [10:12<07:46,
// 466.90s/it]"): false when there is no such log or no bar in its end. Cheap to call every frame (read at most twice a second).
struct ComfyProgress {
    int step = 0, steps = 0;
    double secPerStep = 0; // the bar's own rate
    double leftSeconds = 0;
    int barsDone = 0; // the sampling bars of this job that have finished (a music video has one for each clip)
};
bool ComfyReadProgress(int activityId, ComfyProgress& out); // the running job's, from where it began in ComfyUI's log
// Free system RAM in GB (a job that runs it out slows to a crawl: the model is paged to disk)
double FreeRamGB();
// A small graph (API format, as JSON text) run start to end from a background thread: files uploaded into the given nodes (LoadImage /
// LoadVideo), its outputs downloaded into outDir. Blocks until done, failed, or timeoutSec.
bool ComfyRunGraph(const std::string& url, const std::string& graphJson, const std::vector<std::pair<std::string, std::wstring>>& uploads,
                   const std::wstring& outDir, std::vector<std::wstring>& outFiles, std::string& error, int timeoutSec);
std::wstring ComfyInstallDir(); // the install: Desktop's or a portable ComfyUI's folder (holds ComfyUI\main.py), a git clone's folder (holds main.py); "" = unknown
std::wstring ComfyAppDir();     // the folder main.py is in (custom_nodes and models are in it), whichever kind of install it is; "" = unknown
std::wstring ComfyPython();     // the Python ComfyUI runs on; "" = unknown
void ComfySetFolder(const std::wstring& folder);  // the ComfyUI folder of the settings (comfyFolder): used when ComfyUI Desktop is not installed
bool ComfyFolderValid(const std::wstring& folder, std::string* why); // is it a ComfyUI we can start (main.py and a Python)?
// How long ComfyUI took to make a file, as recorded by this app when it was made (in %LOCALAPPDATA%\Try2Create\generation_times.json),
// and what it was ("Video (Balanced, Veda): watch.png"). False = not known.
bool ComfyGenerationTime(const std::wstring& file, double& seconds, std::string* what = nullptr);
void ComfyMoveGenerationTime(const std::wstring& from, const std::wstring& to); // the file was renamed
std::string FormatTook(double seconds); // "48 s", "3 min 48 s", "1 h 02 min"
std::wstring ComfyInputDir();   // the folder its LoadImage / LoadVideo files are in ("" = unknown)
// Try2Create's browser bridge: a tiny ComfyUI extension (custom_nodes\try2create_bridge, no nodes) with which ComfyUI opened as
// http://.../?try2create=<prompt id> shows that job's workflow - queued, running (with its progress on the nodes) or done.
bool ComfyBridgeInstalled();                    // its files are in ComfyUI's custom_nodes
bool ComfyInstallBridge(std::string& message);  // writes them there (ComfyUI loads it when it next starts)
bool ComfyBridgeLoaded(const std::string& url); // the running ComfyUI has it (GET /extensions). Blocks briefly.
std::wstring ComfyUploadedFrom(const std::string& name); // the file this app uploaded under that name ("" = not one of ours)
// Drama 3 keeps ~35 GB of pinned system RAM that ComfyUI gives back only when it restarts (measured: still held after /free).
bool ComfyHoldsDrama();                                                             // this app sent it a Drama 3 job since it started
bool ComfyGiveBackDramaRam(const ComfySettings& settings, std::string& message);   // restarts the ComfyUI this app started (false: not ours)
int ComfyHasNode(const std::string& url, const std::string& node); // 1 the running ComfyUI has it, 0 not, -1 it is not answering. Blocks briefly.
std::string ComfyHistoryText(const std::string& url, const std::string& promptId); // the text a finished prompt's nodes showed (PreviewAny...), "" = none. Blocks briefly.

// ---- pixel art (the Retro Diffusion models: Stable Diffusion 1.5 checkpoints that draw every art pixel as an 8 x 8 cell)
std::wstring PixelModelsDir(const ComfySettings& settings);        // where the RetroDiffusion*.safetensors files are
std::wstring ComfyModelsDir();                                      // ComfyUI Desktop's models folder ("" = unknown)
// Makes the Retro Diffusion folder visible to ComfyUI: a link (directory junction) models/checkpoints/RetroDiffusion -> that folder (no copy, no admin rights).
bool ComfyLinkPixelModels(const ComfySettings& settings, std::string& message);
// Which Retro Diffusion checkpoint suits pictures of this many art pixels (the longer side): 16x for icons, 24x-64x for sprites, 128x for scenes ("" = none in the list).
std::string PickPixelModel(const std::vector<std::string>& checkpoints, int artSize);
// How many picture pixels one art pixel is in that model's pictures: 16 for the 16x model, 8 for the others.
int PixelModelScale(const std::string& model);
// Can pixel art be drawn by the ComfyUI tab's workflow? (a saved workflow or a file is chosen there: "ComfyUI's last run" could be anything, even one of this app's own jobs)
bool ComfyHasDrawWorkflow(const ComfySettings& settings);
std::string ComfyDrawWorkflowName(const ComfySettings& settings); // the saved workflow picked for drawing, without ".json" ("my_krea_workflow")

bool LoadComfySettings(ComfySettings& settings);
void SaveComfySettings(const ComfySettings& settings);

// Loads the chosen workflow and says where a prompt would go (nothing is sent).
struct ComfyTemplateInfo {
    bool ok = false;
    std::string error;
    int nodes = 0;
    std::string summary; // "prompt: node 6 (CLIPTextEncode); negative: node 7; seed: node 3; input picture: node 10"
};
ComfyTemplateInfo InspectComfyTemplate(const ComfySettings& settings);

// ComfyUI in the background. Only a ComfyUI that this app started is ever closed (with the app, or with ComfyStopOwned).
bool ComfyFindDesktop(std::wstring& exe);
std::wstring ComfyOutputDir(const ComfySettings& settings);                   // where its pictures and videos are saved ("" = unknown)
bool ComfyStart(const ComfySettings& settings, std::string& message);          // launches it; true when started or already ours
bool ComfyOwned();                                                             // did this app start the running ComfyUI?
void ComfyStopOwned();

// Asks ComfyUI to unload its models and free GPU memory (POST /free), for when something else needs the GPU. Blocks briefly.
bool ComfyFreeMemory(const std::string& url, std::string& message);

// Is ComfyUI answering at that address? (short timeout, blocks)
bool ComfyCheck(const std::string& url, std::string& message);

// A workflow saved inside ComfyUI (its user/default/workflows folder), listed through ComfyUI's own API.
struct ComfySavedWorkflow {
    std::string path;       // relative path, e.g. "Qwen image.json" or "tests/flux.json"
    long long modified = 0; // unix seconds (0 = unknown)
};

struct ComfyJob {
    bool quiet = false;     // what it makes is the sending page's own: not on the Made list (it reads them from its activity)
    std::vector<std::string> stages; // for the job tracker: what each sampling pass makes (see ComfyActivity)
    std::string finishing;
    double seconds = 0;     // video prompts: the clip length they were written for (0 = not a video / leave the workflow's)
    int activityId = 0;
    std::string prompt;
    std::string negative;   // "" = leave the workflow's own
    std::wstring picture;   // "" = no input picture
    std::vector<std::wstring> images; // picture EDIT: these go to the built-in Qwen Image 2.1 Edit workflow (the first is the one edited, the rest are
                                      // references; the prompt calls them <image1>, <image2>...) and the workflow chosen in the tab is not used
    std::string label;      // for the messages ("test_a1111.png", "Vision board")
    int canvas = 0;         // picture EDIT: 0 = the first picture is edited (its size and frame), 1 / 2 / 3 = a NEW landscape / square / portrait canvas (the pictures are only references)
    int upscale = 0;        // 0 = a normal edit, 1 = upscale the whole picture, 2 = make the faces better (the first picture only; see BuildUpscale)
    bool anime = false;     // upscaling: the picture is anime / drawn (other models and wording) rather than a photo
    double scale = 2.0;     // upscaling: how many times bigger (1 = same size)
    double detail = 0.3;    // upscaling: how much the MOSS pass may redraw (0 = only resize with an upscale model, no redrawing)
    int steps = 0;          // upscaling: sampler steps of that pass (0 = the edit setting)
    int srcW = 0, srcH = 0; // upscaling: size of the picture (to keep the redraw inside what the GPU can hold)
    int pixel = 0;          // pixel art (Retro Diffusion): 1 = from the prompt, 2 = redraw images[0] (a clean pixel grid at 8x), 3 = redraw only where images[1] (a mask) is white
    int pixW = 0, pixH = 0; // ...its size in art pixels (the picture is made at 8x)
    int cutout = 0;             // 1: SAM 3 cuts what the prompt names ("person, animal") out of images[0]; the result is the mask (white = kept)
    std::string savePrefix;     // with sheetW/H: where ComfyUI saves it (default PixelArt/anim)
    int sheetW = 0, sheetH = 0; // picture EDIT on a canvas of exactly this size (a sprite sheet: the frames side by side), without the edit LoRAs; 0 = as usual
    bool fillMask = false;      // picture EDIT: the last of images is a painted mask (white = redrawn), FLUX 3 fills only there
    std::string editEngine;     // picture EDIT: "flux3" / "qwen" for this job ("" = the editor chosen in the settings)
    int draw = 0;           // pixel art drawn first by a strong model (the app pixelizes it): 1 = the ComfyUI tab's saved workflow (Krea), without its LoRAs,
                            // 2 = Qwen Image 2.1 (text to image). The drawing is about one megapixel, in the shape of pixW x pixH.
                            // 3 = the same Qwen drawing as a picture of its own (no pixel art after), saved under savePrefix.
    double denoise = 0.55;  // ...how much a picture is redrawn (2 and 3)
    double cfg = 7.0;       // ...how closely the prompt is followed
    long long seed = -1;    // ...-1 = a new random one
    int times = 0;          // how many pictures to make from it (0 = the setting "pictures per click")
    int batch = 0;          // jobs sent together share a number: the workflow is read once for all of them
    // pictures for particular LoadImage nodes of the workflow (node id, file): a video's last frame next to its first. `picture` leaves these nodes alone.
    std::vector<std::pair<std::string, std::wstring>> nodePictures;
    std::string apiWorkflow; // a complete API-format workflow of the app's own (music, drama), sent as it is with new seeds (or `seed`): nothing else above applies
    std::vector<std::pair<std::string, std::wstring>> rawUploads; // with apiWorkflow: files uploaded first; an input equal to the placeholder gets the uploaded name
    std::vector<LoraEntry> loras; // put on the model that feeds its sampler(s), after any it already has, just before it is sent
};

struct ComfyResult {
    std::string label, prompt, filename, subfolder, type; // type: "output" / "temp"; prompt: the text that was sent
    std::wstring localPath;                       // the file on this computer ("" = could not be fetched)
    bool video = false;
    bool audio = false;                           // a song or a sound (mp3, flac, wav, opus)
    bool model3d = false;                         // a 3D file (a splat's .ply)
    double seconds = 0;                           // how long ComfyUI took to make it (0 = not known)
};

enum class ComfyState { Waiting, Starting, Sending, Queued, Running, Done, Failed };

// One thing the user sent, from the click to the picture.
struct ComfyActivity {
    int id = 0;
    std::string label;      // what was sent ("test_a1111.png")
    std::string prompt;     // the text that was sent
    std::string workflow;   // which workflow it ran in
    std::string address;    // where it was sent
    ComfyState state = ComfyState::Waiting;
    std::string message;    // what is happening, or why it failed (ComfyUI's own words)
    std::string promptId;   // ComfyUI's id for it, once it is in ComfyUI's queue (the last one of a batch)
    std::vector<std::string> stages; // what each sampling pass makes, in order ("Clip 2 of 5"); empty: "Pass 2 of 3" when there are several
    std::string finishing;  // what comes after the last pass ("Joining the clips"); empty: decoding and saving
    int passes = 0;         // the sampling passes in what was sent to ComfyUI
    int prompts = 0;        // the prompts it became (several takes of one prompt: one each)
    bool quiet = false;     // what it makes is the page's own: not on the Made list
    std::vector<ComfyResult> files;
    long long startedMs = 0, finishedMs = 0; // milliseconds since the app started
    long long runningMs = 0;                 // when ComfyUI began generating it (0 = not yet)
    int pending = 0;                         // prompts of this send still being generated
};

// Runs jobs on a background thread (starting ComfyUI if needed, queueing, then watching ComfyUI's queue and history until each one is done).
class ComfyService {
public:
    ComfyService();
    ~ComfyService();
    ComfyService(const ComfyService&) = delete;
    ComfyService& operator=(const ComfyService&) = delete;

    int Send(const ComfySettings& settings, ComfyJob job); // the id of its entry in Activity()
    // Stops a send: not sent yet - dropped; in ComfyUI's queue - taken out of it; generating - interrupted (only that prompt, never someone else's).
    void Cancel(int activityId);
    std::vector<std::string> TakeMessages();       // short messages for the status line
    std::vector<ComfyActivity> Activity() const;   // everything sent this session, oldest first
    std::vector<ComfyResult> Results() const;      // finished pictures and videos, newest last
    // Takes results off the list (the files stay): all of them, or only the ones whose file is gone (moved or deleted). The number taken off.
    int ClearResults(bool onlyMissing);
    void RemoveResult(const std::wstring& path); // this one file off the list
    void RestoreResult(const ComfyResult& r);    // back on the list (out of the Recycle bin page)
    int Active() const { return active_.load(); }  // jobs queued or still generating
    void SetAddress(const std::string& url);       // the address to keep an eye on (is it up?)
    bool Reachable() const { return up_.load(); }
    std::string Version() const;
    static long long NowMs();                      // the clock used by ComfyActivity

    // The workflows saved in ComfyUI: refreshed in the background (and, with autoImport, copied into the Workflows tab).
    void RefreshWorkflows(const ComfySettings& settings);
    std::vector<ComfySavedWorkflow> SavedWorkflows() const;
    bool Refreshing() const { return refreshing_.load(); }
    std::string ListMessage() const;               // why the list is empty / how many were imported

private:
    struct Queued {
        ComfySettings settings;
        ComfyJob job;
        std::string model; // the model files it loads (ModelKeyOf): jobs on the model ComfyUI has loaded go first
        int skipped = 0;   // times a later job on the loaded model went before it (at most 3)
    };
    int WaitingInComfyLocked() const; // our jobs sitting in ComfyUI's queue, not running yet (under mutex_)
    struct Watching {
        ComfySettings settings;
        std::string id;
        int activityId = 0;
        long long startedMs = 0;
        long long seenMs = 0; // when ComfyUI last listed it as queued or running (or could not be asked)
    };
    void Loop();
    void Process(Queued& q);
    void Poll();
    void Ping();
    void Say(const std::string& m);
    void Update(int activityId, ComfyState state, const std::string& message);
    void Unlabel(int activityId, const std::string& part); // takes `part` out of its label
    void FinishPrompt(int activityId, bool ok, const std::string& message, std::vector<ComfyResult> files);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Queued> queue_;
    std::vector<Watching> watching_;
    std::vector<std::string> messages_;
    std::vector<ComfyActivity> activity_;
    std::vector<ComfyResult> results_;
    std::vector<ComfySavedWorkflow> saved_;
    std::string listMessage_;
    std::string address_, version_;
    long long lastPingMs_ = 0;
    int nextActivity_ = 0;
    std::set<int> cancelled_; // sends stopped while they were being sent (their prompt is taken back once ComfyUI names it)
    std::string lastModel_;   // the model files of the last job sent
    std::thread refreshThread_;
    std::atomic<bool> refreshing_{false};
    std::atomic<bool> up_{false};
    std::atomic<int> active_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
