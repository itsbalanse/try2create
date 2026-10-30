#pragma once
#include <memory>
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>
#include <future>
#include <filesystem>
#include <d3d11.h>
#include "metadata.h"
#include "image_texture.h"
#include "disk_cache.h"
#include "scrape.h"
#include "nsfw_classifier.h"
#include "aesthetic_scorer.h"
#include "organizer.h"
#include "vision_prompt.h"
#include "image_attributes.h"
#include "image_hash.h"
#include "person_detector.h"
#include "folder_watcher.h"
#include "metadata_strip.h"
#include "workflow.h"
#include "board.h"
#include "video_player.h"
#include "comfy.h"
#include <condition_variable>
#include <deque>

struct ImageEntry {
    std::wstring path;
    std::wstring displayName;
    std::filesystem::file_time_type mtime{};
    uintmax_t size = 0;

    // Filled in asynchronously by the background loader; only read `metadata`
    // /thumbnail fields on the UI thread after checking thumbReady. thumbSrv
    // is a non-owning pointer: AppState's thumbnail cache owns the lifetime.
    ID3D11ShaderResourceView* thumbSrv = nullptr;
    int thumbW = 0;
    int thumbH = 0;
    ImageMetadata metadata;
    std::string searchTextLower; // prompt + filename, lowercased, for the search filter
    bool thumbReady = false;

    // NSFW scores in [0,1]; negative = unknown. The prompt score is computed
    // from the positive prompt as soon as metadata loads (negative if there's
    // no prompt); the image score comes from the classifier model (or the
    // persisted score cache) once the image has been classified.
    float nsfwPromptScore = -1.0f;
    float nsfwImageScore = -1.0f;
    bool clefGraded = false; // the NSFW image score, quality and style come from Clef (clef_grades.cpp), not from the small classifier models

    // Picture quality (aesthetic models), 0..10; negative = not scored yet. See AestheticScorer.
    float qualityAnime = -1.0f, qualityGeneral = -1.0f, qualityAnimeProb = -1.0f;
    int styleKind = -1;         // what kind of picture it is (index into kStyleNames; negative = not classified yet)
    float styleConf = -1.0f;
    AestheticScorer::Scores QualityScores() const {
        AestheticScorer::Scores s;
        s.anime = qualityAnime;
        s.general = qualityGeneral;
        s.animeProb = qualityAnimeProb;
        return s;
    }
    float Quality() const { return qualityGeneral < 0 && qualityAnime < 0 ? -1.0f : QualityScores().Quality(); }

    // Facts for sorting/filtering. year/month come from the file's modified
    // time (known right away); the rest are derived once metadata has loaded.
    int year = 0, month = 0;
    int people = -1;          // people in the image: detected (peopleFromImage) or estimated from the prompt; -1 = unknown, 0 = none
    bool peopleFromImage = false;
    int genW = 0, genH = 0;   // generation size from the "Size" parameter (0 = unknown)
    std::string modelLower;   // "Model" parameter, lowercased, for sorting by checkpoint
    int rating = 0;           // 0 = unrated, 1-5 stars (persisted in ratings.json)
    bool missing = false;     // the file was deleted/moved away while the folder was open
    unsigned long long arrivedTick = 0; // GetTickCount64() when it appeared while the folder was open (a "NEW" badge until it is looked at); 0 = was there already
    int visionMask = 0;       // already described with Vision: bit 1 image prompt, 2 tags, 4 video scene
    int helper = 0;           // a helper picture, not a result (HelperKind: depth map, mask, pose): the gallery hides these
};

// The score used for badges/sorting/filtering: Combined takes the higher of
// the two (either one flagging an image is enough); negative = unknown.
float EffectiveNsfwScore(const ImageEntry& e, NsfwSource source);


// One image file found while listing a folder.
struct ListedImage {
    std::wstring path;
    std::wstring relative; // path relative to the listed folder
    std::filesystem::file_time_type mtime{};
    uintmax_t size = 0;
};

// Lists the supported image files under `folder`. A directory inside
// `excludeDir` is skipped (this is how the Vision output folder stays out of
// the gallery), unless `folder` itself is inside `excludeDir`.
std::vector<ListedImage> ListImageFiles(const std::wstring& folder, bool recursive, const std::wstring& excludeDir,
                                        const std::atomic<bool>* cancel = nullptr, std::atomic<int>* progress = nullptr);

// Drops copies of the same image: files with the same name, size and modified
// time (what copying preserves). Keeps the one outside `excludeDir`, then the
// one closest to the top. Returns how many were dropped.
int DropDuplicateCopies(std::vector<ListedImage>& files, const std::wstring& excludeDir);

// The "Vision board" tab: cards on a canvas -> one scene prompt.
struct BoardViewState {
    Board board;
    bool loaded = false;
    std::string name;            // saved board's name ("" = the scratch board)
    int selNode = -1, selLink = -1;
    int addKind = 0;             // library panel: add pictures as Character / Background / Item
    std::string search;          // library panel search
    bool autoDescribe = true;    // new pictures are described by the vision model
    bool dirty = false;
    double dirtyAt = 0;
    bool fitRequest = true;
    // prompt panel
    int format = 0, seconds = 10;
    int styleIdx = 0;            // 0 none, 1..N the VisionStyle presets, N+1 custom
    std::string customStyle, extra;
    bool attach = false;
    std::string result;
    std::string resultDraft, pendingDraft; // the quick draft when the final prompt was written (to tell when it is out of date)
    bool writing = false;
    bool editResult = false;
    // canvas interaction
    int dragNode = -1, resizeNode = -1, linkFrom = -1, ctxNode = -1, ctxLink = -1;
    int resizeEdges = 0;         // which sides of a card are being dragged: 1 left, 2 right, 4 top, 8 bottom
    int editLink = -1;           // a relation whose text is being rewritten in place (double click on it)
    std::string editLinkBuf;
    bool editLinkFocus = false;
    int swapNode = -1;           // the card the "swap with another character" popup is for
    bool openSwap = false, swapKeepGear = false;
    bool relGroups = true;       // relation lists grouped by category (Fighting, Friends & team, Couple, Funny...)
    bool linkFromOutput = true, panning = false;
    float dragMoved = 0;
    float centerX = 300, centerY = 200; // middle of the visible board (board units), where "add" puts a card
    double idleSince = 0;
    std::string saveName;
    // scenes, timeline, relation picker
    int curScene = 0;            // the scene the canvas shows
    int renameScene = -1;
    std::string sceneBuf;
    int relPickLink = -1;        // a link just made: the popup that asks what happens
    std::string customRel;
    bool showTimeline = true;
    // timeline editor
    float tlStart = 0, tlLen = 0;    // visible window (seconds); tlLen 0 = fit
    float playhead = 0;
    bool tlSnap = true, tlRazor = false;
    int tlMode = 0;                  // 0 none, 1 scrub, 2 move, 3 trim start, 4 trim end, 5 pan
    int tlGrabScene = -1;
    float tlGrabDx = 0, tlOrigStart = 0, tlOrigEnd = 0;
    float tlMoved = 0;               // pixels dragged since the mouse went down (a click is not a drag)
    int tlReveal = -1;               // scroll / zoom the timeline so this scene is in view
    // undo / redo (snapshots of the board, taken once an edit is finished)
    std::vector<Board> undoStack, redoStack;
    Board lastSnapshot;          // the board as of the last finished edit
    bool histDirty = false;      // something changed since lastSnapshot
    std::string iconSearch;
    int iconCat = 0;
    bool libShowIcons = false;   // left panel shows the built-in icons
    bool libShowChars = false;   // left panel shows the saved characters instead of the library pictures
    bool invQuick = false, invOpenUrl = false, invOpenSaved = false; // open an inventory "add" popup from the card's + slot
    bool invFocus = false;       // scroll the side panel to the inventory
    int airFrom = -1;            // a link was dropped on empty canvas: the card it came from...
    bool airOutput = true;       // ...from its right dot (true) or left dot
    float airX = 0, airY = 0;    // ...and where (board units)
    bool openAir = false, openRelPick = false;
    int airOpenFrame = 0;        // frame it was opened in (a click on that frame is the one that opened it)
    int airTarget = -1;          // the card a link was dropped on (-1 = dropped on empty canvas)
    bool airOpen = false;        // the "add something" panel shown after a link is dropped on empty canvas
    float airScreenX = 0, airScreenY = 0;
    int airKind = 0;             // tab of that panel (BoardKind)
    // inventory
    std::string itemUrl, itemUrlName;
    std::future<std::pair<std::wstring, std::string>> urlJob; // (picture file, error) of a running web download
    std::string urlJobKey;       // character the download is for
    int urlJobSlot = 1;
    std::string urlJobName;
    std::vector<SavedItem> savedItems; // the items folder, refreshed when its popup opens
    bool profilesDirty = false;
    double profilesDirtyAt = 0;
    // boards browser
    bool showBrowser = false;
    std::string browserSearch, browserChar; // browserChar = character key to filter by ("" = all boards)
    std::vector<BoardInfo> browserBoards;
    bool browserDirty = true;
    std::wstring deletePath;
    std::wstring prevBoardPath;  // the board that New / Open replaced: the Back button reopens it
    std::string prevBoardName;
    // "prepare for generation"
    std::string exportDir;       // "" = DefaultExportDir()
    std::wstring lastExportFolder;
    std::vector<std::wstring> lastExportFiles;
};

class AppState {
public:
    explicit AppState(ID3D11Device* device) : device_(device) {
        // The thumbnail/metadata index can be 100+ MB of JSON: parse it in the
        // background so the window appears at once. The folder loader waits
        // for it before touching the cache.
        diskCacheReady_ = std::async(std::launch::async, [this]() { diskCache_.Load(); }).share();
        LoadNsfwCache();
        LoadQualityCache();
        LoadRatings();
        LoadPHashCache();
        LoadPeopleCache();
        RefreshRenameLog();
        LoadOrganizeSettings();
        LoadVisionSettings();
        LoadUiSettings();
    }
    ~AppState();

    // Starts scanning `folder` in the background (the window stays responsive; IsListing()
    // is true until the list arrives). `selectAfter`: full path of an image to select once loaded.
    void OpenFolder(const std::wstring& folder, const std::wstring& selectAfter = L"");
    bool IsListing() const { return listing_.load(); }
    int ListingFound() const { return listingFound_.load(); }
    bool EntriesSortedByName() const { return entriesSortedByName_; } // lets the gallery skip re-sorting
    void OpenFile(const std::wstring& file); // loads the file's parent folder and selects it
    // resetMultiSelect: pass false when the caller (e.g. a Ctrl/Shift-click
    // handler) is managing `multiSelected` itself and just wants the viewer
    // to follow along.
    void SelectIndex(int index, bool resetMultiSelect = true);
    // Steps through the gallery as currently filtered/sorted (visibleOrder),
    // wrapping at the ends. Falls back to all entries if no order is known.
    void SelectRelative(int delta);
    void SelectNext() { SelectRelative(1); }
    void SelectPrev() { SelectRelative(-1); }
    void SelectFirst();
    void SelectLast();
    // Re-lists the current folder and keeps the current image selected.
    void ReloadFolder(const std::wstring& selectPath = L""); // selectPath: image to select afterwards (default: the current one)
    void SetStatus(const std::string& msg);
    // True while the last status message is recent enough to display.
    bool StatusActive() const;

    // UI preferences + recent folders (persisted in %LOCALAPPDATA%).
    void LoadUiSettings();
    void SaveUiSettings();

    // Star ratings (1-5, 0 clears) for the given gallery indices; persisted per file path.
    void SetRating(const std::vector<int>& indices, int rating);
    void SaveRatingsIfDirty(bool force = false); // call each frame; writes a moment after the last change

    // Saved searches and recent search history (persisted).
    void PushSearchHistory(const std::string& query);

    // Restricts the gallery to the given entries (e.g. one group of similar
    // images) until cleared.
    void SetRestrict(const std::vector<int>& indices);
    void ClearRestrict();

    // Side-by-side comparison of two gallery entries (textures owned here).
    void OpenCompare(int a, int b);
    void CloseCompare();

    // Near-duplicate scan over the current gallery (background thread).
    void StartSimilarScan(int maxDistance);
    void CancelSimilarScan();
    bool IsSimilarScanning() const { return similarRunning_.load(); }
    int SimilarCurrent() const { return similarCurrent_.load(); }
    int SimilarTotal() const { return similarTotal_.load(); }
    void PumpSimilarResults(); // call each frame

    // Counts the people actually visible in images with an object-detection
    // model (models\people.onnx), in the background. Results override the
    // prompt-based estimate and are cached per file.
    void StartPeopleScan(const std::vector<int>& indices);
    void CancelPeopleScan();
    bool IsPeopleScanning() const { return peopleRunning_.load(); }
    int PeopleCurrent() const { return peopleCurrent_.load(); }
    int PeopleTotal() const { return peopleTotal_.load(); }
    void PumpPeopleResults(); // call each frame

    // Clean copies without embedded metadata (background job).
    void StartCleanExport(const std::vector<int>& indices);
    void CancelCleanExport();
    bool IsCleanExporting() const { return cleanRunning_.load(); }
    int CleanCurrent() const { return cleanCurrent_.load(); }
    int CleanTotal() const { return cleanTotal_.load(); }
    void PumpCleanExport();
    bool showCleanExport = false;
    std::string cleanExportDir; // UTF-8; empty = "<opened folder>\\clean"
    int cleanMode = 0;          // 0 = remove everything, 1 = keep the prompt only (PNG)
    bool cleanOpenWhenDone = true;
    std::string cleanMessage;

    // Batch rename from metadata (background job with an undo log).
    void StartRename(const std::vector<std::pair<std::wstring, std::wstring>>& moves);
    void StartUndoRename();
    void CancelRename();
    bool IsRenaming() const { return renameRunning_.load(); }
    int RenameCurrent() const { return renameCurrent_.load(); }
    int RenameTotal() const { return renameTotal_.load(); }
    bool CanUndoRename() const { return !latestRenameLog_.empty(); }
    void PumpRename();
    bool showRename = false;
    std::string renameTemplate = "{date}_{model}_{seed}";
    int renameStart = 1;
    int renameScope = 0; // 0 = selected images, 1 = everything in the current view
    std::string renameMessage;

    // Keeps ratings, scores, counts and thumbnails attached to a file that was renamed or moved.
    void MovePathMetadata(const std::wstring& from, const std::wstring& to);

    // Which images already have a Vision result: the output folder's .txt files plus this session.
    void PumpDescribed(); // call each frame
    void MarkDescribedDirty() {
        describedDirty_ = true;
        describedDirtyTick_ = GetTickCount64();
        libDirty_ = true;
        libDirtyTick_ = describedDirtyTick_;
    }
    static std::string VisionMaskLabel(int mask); // "image prompt, tags, video scene"

    // ---- Moving pictures into folders (gallery and Vision library) ----
    struct MoveReport {
        int moved = 0, failed = 0, texts = 0;
        std::string message;
    };
    // Moves the image files into `destDir` (made if needed), with the result texts that sit next to them
    // (<name>.txt, .tags.txt, .video-Ns.txt, .style-x.txt). Names that are taken get " (2)". Everything the
    // program remembers per file (ratings, scores, thumbnails) follows.
    MoveReport MoveImages(const std::vector<std::wstring>& imagePaths, const std::wstring& destDir);
    bool CreateFolder(const std::wstring& parent, const std::string& name, std::string* error);
    unsigned folderGeneration = 0; // bumped whenever folders are made or files moved (the folder tree rebuilds)
    // ---- Vision library: the output folder's results (image + prompt + tags + video prompts), in their own tab ----
    struct VisionLibItem {
        std::vector<std::pair<std::string, std::string>> styled; // <name>.style-<style>.txt as (style, text)
        std::wstring imagePath;
        std::wstring name;
        uintmax_t size = 0;
        std::filesystem::file_time_type mtime{};
        std::filesystem::file_time_type resultTime{};     // when its newest result text was written
        std::string dir;                                  // folder below the output folder ("" = top level)
        std::string prompt;                               // <name>.txt
        std::string tags;                                 // <name>.tags.txt
        std::vector<std::pair<int, std::string>> videos;  // <name>.video-<N>s.txt as (seconds, text), ascending
        std::string searchLower;                          // name + every text, for the search box
    };
    struct LibThumb {
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
        bool failed = false;
        unsigned long long stamp = 0;     // the file's time and size when it was loaded (a new file under the same name is loaded again)
        unsigned long long checkedMs = 0; // when the file was last looked at
    };
    void RequestVisionLibrary(); // the tab is showing: load (or refresh) the list when needed
    void RefreshVisionLibrary() { libDirty_ = true; libDirtyTick_ = 0; }
    void PumpVisionLibrary();    // call each frame
    bool VisionLibraryScanning() const { return libScanning_.load(); }
    void SelectVisionLib(int index);
    LibThumb LibraryThumb(const std::wstring& imagePath); // small preview; loads in the background (srv null until ready); blurred when NSFW and blurNsfw
    LibThumb LibraryThumbRaw(const std::wstring& imagePath); // the same, never blurred
    // Folders inside the Vision output folder (relative, backslash separated, utf-8) and the one being looked at.
    std::vector<std::string> visionLibDirs;
    std::string visionLibDir;          // "" = everything
    bool visionLibTopOnly = false;     // only the images that are not in a folder yet
    bool visionLibFolders = true;      // show the folder pane
    bool visionLibRedoRequest = false; // the context menu asked for the "describe again" box
    std::vector<VisionLibItem> visionLib;
    unsigned visionLibVersion = 0; // bumped whenever visionLib is replaced
    std::string visionLibFolder;   // folder the list was read from
    bool visionLibLoaded = false;
    int visionLibSelected = -1;
    LoadedImage visionLibImage;    // full-size picture of the selected item
    std::wstring visionLibImagePath;
    // Playing a video inside the viewer (volume/loop/autoplay persist)
    std::unique_ptr<VideoPlayer> videoPlayer;
    float videoVolume = 0.8f;
    bool videoMuted = false, videoLoop = true, videoAutoplay = false;
    void PlayVideo(int index);
    void StopVideo();
    bool UpdateVideo(); // each frame; true while the picture is moving
    bool VideoOpen() const { return videoPlayer && videoPlayer->IsOpen(); }
    WorkflowViewState wf;          // the "Workflows" tab: node viewer + library
    BoardViewState board;          // the "Vision board" tab
    ComfySettings comfy;           // sending prompts to a running ComfyUI
    bool comfyLoaded = false;
    std::unique_ptr<ComfyService> comfySvc;
    ID3D11Device* Device() const { return device_; }
    int activeTab = 0;             // 0 gallery, 1 Vision library, 2 Workflows, 3 Vision board, 4 ComfyUI (set while drawing) - the focused page
    int requestTab = -1;           // set to switch tabs from anywhere (with tiling on: it opens beside the page you are on)
    bool requestAlone = false;     // ...and with this, it takes the whole width instead
    // Tiling (like Hyprland): up to two pages side by side - opening another page splits the screen; tiles[1] = -1 when one page fills it
    bool tilePages = true;         // (persisted)
    int tiles[2] = {0, -1};
    bool tilePinned[2] = {false, false}; // a pinned tile is never replaced by a page you open
    bool tileMin[2] = {false, false};    // a minimised tile: a thin strip at its side (the other page gets the room)
    bool TabShown(int t) const { return tiles[0] == t || tiles[1] == t; }

    // What the gallery leaves out (persisted): the Vision output folder, and extra copies of
    // the same image. HiddenCopies() is how many copies the last folder load dropped.
    bool hideVisionFolder = true;
    bool hideDuplicateCopies = true;
    int HiddenCopies() const { return hiddenCopies_; }

    // Describe every image in a folder with the vision model (listing happens in the
    // background; images already described into the output folder can be skipped).
    void StartVisionFolder(const std::wstring& folder, bool recursive);
    void CancelVisionFolder();
    bool IsVisionFolderScanning() const { return visionFolderBusy_.load(); }
    bool visionSkipDone = true;
    // A style the image prompt is re-imagined in: "" = describe it as it is, a preset key, or "custom".
    std::string visionStyle, visionStyleCustom;
    std::string StyleDirective() const; // text sent to the model ("" = none)
    std::string StyleSlugKey() const;   // file-name part ("anime", "custom-dark-fantasy")
    // Describing many images at once asks first (turn off with "Don't ask again" or in the Vision window).
    bool visionWarnBig = true;
    int visionWarnOver = 100;
    std::vector<std::wstring> visionPending; // images waiting for the "describe many?" answer
    bool showVisionConfirm = false;
    int CountVisionTodo(const std::vector<std::wstring>& paths) const; // images that would really be sent (skipping finished ones)
    void RequestVisionBatch(const std::vector<int>& indices);          // gallery entries: asks first when it is a lot, else starts
    void RequestVisionPaths(const std::vector<std::wstring>& paths);   // the same for any image files (e.g. the Vision library)
    void DescribePaths(const std::vector<std::wstring>& paths);
    std::wstring SaveDirFor(const std::wstring& imagePath, const std::wstring& batchDir) const; // an image already in the output folder is described in place        // describe these files now, as the Vision settings say

    // Live folder watching: new images appear (and removed ones disappear) while
    // the folder is open, without a manual reload.
    void PumpFolderWatch(); // call each frame
    int LiveCount() const { return static_cast<int>(entries.size()) - missingCount_; } // images not known to be gone
    bool watchFolder = true;   // persisted
    bool followNewest = false; // jump to each new image as it arrives; persisted
    void ForgetDetectedPeople(); // go back to prompt-based estimates
    int DetectedPeopleCount() const; // entries with an image-based count
    bool PeopleModelAvailable() const { return !PersonDetector::ModelId().empty(); }
    std::string peopleMessage;
    std::string peopleDevice; // "DirectML (GPU)" / "CPU" after the first scan
    bool showPeopleDetect = false;
    void AddRecentFolder(const std::wstring& folder);

    // Must be called once per frame from the UI thread: applies any
    // thumbnails/metadata the background loader has finished decoding.
    void PumpBackgroundResults();

    bool IsLoading() const { return loaderActive_.load(); }
    int LoadedCount() const { return loadedCount_.load(); }
    int TotalCount() const { return totalCount_; }
    void CancelLoading();

    // Runs ScrapeFolder/ScrapeFiles on a background thread so a huge folder
    // doesn't freeze the UI the way folder loading used to before it got the
    // same treatment. Only one scrape runs at a time; starting a new one
    // cancels/joins any previous one first.
    void StartScrapeFolder(const std::wstring& folder, bool recursive, ScrapeFormat format,
                           const std::wstring& outputFile);
    void StartScrapeFiles(const std::vector<std::wstring>& files, ScrapeFormat format, const std::wstring& outputFile);
    void CancelScrape();
    bool IsScraping() const { return scrapeRunning_.load(); }
    int ScrapeCurrent() const { return scrapeProgress_.current.load(); }
    int ScrapeTotal() const { return scrapeProgress_.total.load(); }
    // Call once per frame; when the background scrape finishes, copies its
    // result message into whichever of scrapeResultMessage /
    // exportSelectedResultMessage started it.
    void PumpScrapeResult();

    enum class ScrapeKind { None, Folder, Selected };

    // Background NSFW jobs (one at a time, sharing the progress below):
    //  - StartNsfwScoreJob: model-scores gallery entries (cached by
    //    path/mtime/size, so re-runs only process new files).
    //  - StartOrganize / StartUndoOrganize: see organizer.h. Rescans the
    //    current folder afterward if any files moved.
    void StartNsfwScoreJob(const std::vector<int>& indices);
    void StartOrganize(const OrganizeOptions& opts);
    void StartUndoOrganize();
    // Organize options from the current dialog settings (scoring, videos,
    // duplicates, ...) for the given source/destination.
    OrganizeOptions MakeOrganizeOptions(const std::wstring& source, const std::wstring& dest, bool dryRun) const;
    void CancelNsfwJob();
    bool IsNsfwJobRunning() const { return nsfwRunning_.load(); }
    int NsfwJobCurrent() const { return nsfwProgress_.current.load(); }
    int NsfwJobTotal() const { return nsfwProgress_.total.load(); }
    const char* NsfwJobPhase() const;
    // Call once per frame: applies finished scores to entries, and reports /
    // rescans when a job completes.
    void PumpNsfwResults();

    // ---- Picture quality scoring (aesthetic models) ----
    void StartQualityJob(const std::vector<int>& indices); // scores the ones not scored yet
    void CancelQualityJob();
    bool IsQualityRunning() const { return qualityRunning_.load(); }
    int QualityCurrent() const { return qualityCurrent_.load(); }
    int QualityTotal() const { return qualityTotal_.load(); }
    void PumpQualityResults();
    static bool QualityModelsAvailable() { return AestheticScorer::ModelsPresent(); }
    void SaveOrganizeSettings();

    // Vision prompts: describes the given entries with the selected Ollama
    // vision model (copying each image + .txt to visionOutputDir when
    // auto-save is on). Results land in visionTexts via PumpVisionResults.
    void DescribeWithVision(const std::vector<int>& indices);
    void PumpVisionResults();
    std::vector<VisionService::Result> composeResults; // answers to Vision board requests (taken by the board tab)
    void SaveVisionSettings();
    OllamaEndpoint VisionEndpoint() const { return OllamaEndpoint{}; }

    ID3D11Device* device_;
    std::wstring currentFolder;
    std::vector<ImageEntry> entries;
    int selectedIndex = -1;

    LoadedImage currentTexture;
    ImageMetadata currentMetadata;

    float zoom = 1.0f;
    bool fitToWindow = true;
    float panX = 0.0f, panY = 0.0f; // image-centre offset from the viewer centre, in pixels

    // Indices into `entries` in the order the gallery currently shows them
    // (after search filter and sort). Rebuilt every frame by the gallery.
    std::vector<int> visibleOrder;
    unsigned filterEpoch = 0;      // bumped whenever entries/scores change, so the gallery knows to re-filter
    int galleryColumns = 1;        // columns in the gallery grid, for Up/Down navigation
    bool scrollToSelected = false; // gallery should bring the selected cell into view
    bool gridToTop = false;        // the gallery goes back to its top
    // Where you were in the gallery before a jump (a picture opened from another page, Top): the Back button goes there again
    struct GalleryPlace {
        std::wstring folder, path;
        float scrollY = 0.0f;
    };
    std::vector<GalleryPlace> galleryBack;
    float gridScrollNow = 0.0f;    // how far down the gallery is
    int gridFindFrames = 0;        // a jump to a picture: the gallery centres it, for these frames (a gallery just opened has no size yet)
    float gridScrollTo = -1.0f;    // goes there on the next frame
    void GalleryRemember();
    void GalleryGoBack();
    std::vector<std::string> recentFolders; // UTF-8, most recent first

    // Last window position/size (restored at startup), written by main().
    bool hasWinPlacement = false;
    bool winMaximized = false;
    int winX = 0, winY = 0, winW = 0, winH = 0;

    std::string statusMessage;
    unsigned long long statusSetTick = 0;
    std::string searchQuery; // tag/prompt filter for the gallery, AND-matched, comma/space separated
    // Search by MEANING (the embedding model, embed_index.cpp): the typed text, or a picture ("more like this"), becomes a vector and the gallery shows the pictures closest to it.
    bool semanticOn = false;
    std::string semanticText;          // what is being searched ("a knight in the rain", "like cat.png")
    std::vector<float> semanticVec;    // its vector (empty = not there yet)
    std::wstring semanticLikePath;     // "more like this": the picture whose own vector is waited for
    unsigned semanticVersion = 0;      // bumped when the query changes
    float semanticMin = 0.50f;         // pictures less similar than this are hidden
    std::vector<float> semanticScore;  // the similarity of each picture of the folder to the query (-2 = not indexed yet), as of the last filtering
    bool visionLibBoxes = false;    // Vision library: thumbnails in boxes (like the gallery) instead of the list
    float visionLibBoxSize = 130.0f;
    bool showFolderPane = false;    // gallery: the folder tree to the left (persisted)
    bool navCollapsed = false;      // the page sidebar shows only its icons (persisted)
    bool galleryHideViewer = false, galleryHideInfo = false; // the gallery's viewer / prompt-and-info panes folded to strips (persisted)
    bool showUnreadable = false;    // gallery: also list files that could not be read as pictures
    int hiddenUnreadable = 0;       // how many the current view leaves out (set while filtering)
    int hiddenHelpers = 0;          // depth maps, masks and poses left out (set while filtering)
    int metadataMode = 1;           // gallery: 0 all images, 1 only with generation metadata, 2 only WITHOUT metadata
    float visionLibImageFrac = 0.42f; // Vision library: share of the reader the picture takes (the rest is the prompt text)
    bool visionLibSideBySide = false; // Vision library: picture beside the text instead of above it
    float thumbnailSize = 100.0f; // gallery thumbnail cell size in pixels, user-adjustable

    enum class SortMode { Name, DateModified, FileSize, Generator, NsfwScore, People, Model, Resolution, Rating, Quality, Style };
    SortMode sortMode = SortMode::Name;
    bool sortDescending = false;

    // Quick filters shown in the gallery's "Filters" section (all AND-ed with the search box).
    struct GalleryFilters {
        static constexpr int kAny = -1;
        static constexpr int kUnknownPeople = -2;
        int generator = kAny;   // Generator enum value
        int year = 0;           // 0 = any
        int month = 0;          // 0 = any, 1-12
        int people = kAny;      // 0, 1, 2, 3, 4 (= 4 or more), kUnknownPeople
        int orientation = 0;    // 0 any, 1 portrait, 2 landscape, 3 square
        int minRating = 0;      // 0 any, 1-5 = at least that many stars, -1 = unrated only
        std::string folder;     // "" any, "." = files directly in the opened folder, else a first-level subfolder
        int vision = 0;         // 0 any, 1 already described with Vision, 2 not described yet
        int kind = 0;           // 0 any, 1 images only, 2 videos only
        int style = 0;          // 0 any, -1 not classified yet, else kind + 1 (see kStyleNames)
        int minQuality = 0;     // 0 any, -1 not scored yet, else at least this quality (1-9)
        int pixel = 0;          // 0 any, 1 hide pixel art (and what the Pixel art tab made on the way), 2 only pixel art
        std::string dir;        // "" any, else a folder below the opened one (backslash path, utf-8): that folder and everything in it
        int ActiveCount() const {
            return (generator != kAny) + (year != 0) + (month != 0) + (people != kAny) + (orientation != 0) +
                   (minRating != 0) + !folder.empty() + (vision != 0) + (kind != 0) + !dir.empty() + (minQuality != 0) + (style != 0) + (pixel != 0);
        }
        void Clear() { *this = GalleryFilters(); }
        bool operator==(const GalleryFilters& o) const {
            return generator == o.generator && year == o.year && month == o.month && people == o.people &&
                   orientation == o.orientation && minRating == o.minRating && folder == o.folder && vision == o.vision && kind == o.kind && dir == o.dir && minQuality == o.minQuality && style == o.style && pixel == o.pixel;
        }
    };
    GalleryFilters filters;

    // View options (persisted)
    bool scanSubfolders = true;     // include subfolders when opening a folder
    bool showHelpers = false;       // depth maps, masks and poses show in the gallery too
    bool showGroupHeaders = true;   // section headers in the gallery for the current sort
    bool lightTheme = false;
    int accent = 0; // the accent colour (ui::AccentName: Violet, Blue, Teal...)
    float uiScaleMul = 1.0f;        // user UI scale on top of the monitor's DPI scale

    // Search extras
    std::vector<std::pair<std::string, std::string>> savedSearches; // name, query
    std::vector<std::string> searchHistory;                         // most recent first
    std::string pendingSearchText; // set to push text into the search box
    bool hasPendingSearch = false;

    // Gallery restriction (see SetRestrict)
    std::vector<char> restrictMask;
    bool restrictActive = false;
    unsigned restrictVersion = 0;
    std::string restrictLabel;

    // Compare window
    bool showCompare = false;
    int compareA = -1, compareB = -1;
    LoadedImage compareTex[2];

    // Slideshow
    bool slideshowActive = false;
    float slideshowSeconds = 5.0f;
    double slideshowNextTime = 0.0;

    // Extra windows
    bool showStats = false;
    bool showAppearance = false;
    bool showSimilar = false;
    int similarMaxDistance = 4;
    std::vector<std::vector<int>> similarGroups; // gallery indices, largest group first
    std::string similarMessage;

    bool requestFocusSearch = false; // set by the Ctrl+F shortcut; DrawThumbnailGrid consumes it
    bool requestClearSearch = false; // set by the Esc shortcut

    // Gallery multi-select (Ctrl+click toggles, Shift+click selects a range).
    std::set<int> multiSelected;
    int lastClickedIndex = -1;

    // Delete confirmation (Recycle Bin, reversible). Populated when the user
    // presses Delete or clicks "Delete Selected"; the confirm popup reads it.
    bool showDeleteConfirm = false;
    std::vector<std::wstring> pendingDeletePaths;

    // Batch-export-selected dialog state (shares scrapeFormat with the
    // folder-wide Scrape dialog).
    bool showExportSelectedDialog = false;
    std::string exportSelectedResultMessage;

    // Random Danbooru tag generator dialog state
    bool showTagGeneratorDialog = false;
    bool tagGenInitialized = false; // seeds tagGenEnabledCategories from defaults on first open
    std::set<std::string> tagGenEnabledCategories;
    int tagGenCount = 15;
    std::vector<std::string> generatedTags;

    // Tags imported from a previously-scraped CSV/JSON file, added to the
    // random generator's pool alongside the built-in categories.
    std::vector<std::string> importedTags;
    bool tagGenUseImported = true;
    std::string tagImportResultMessage;

    // NSFW classifier / sorter
    NsfwSource nsfwSource = NsfwSource::Combined;
    bool showNsfwBadges = true;
    // Blur pictures classified as NSFW (score at or above nsfwThreshold), everywhere pictures show; "Show it" reveals one for this session.
    bool blurNsfw = false;
    int charView = 0;          // Characters tab, Your characters: 0 list, 1 grid, 2 large grid
    float charTile = 96.0f;    // the grid's tile width (dp; Ctrl + mouse wheel)
    std::set<std::wstring> nsfwRevealed; // lowercased paths
    // Your own call on a picture or video (a person's eye beats the classifier): 1 NSFW (always blurred),
    // 0 not (never blurred), -1 none (its score decides). Kept in nsfw_marks.json.
    int NsfwMark(const std::wstring& path);
    void SetNsfwMark(const std::wstring& path, int mark);
    bool IsNsfw(const std::wstring& path); // your mark, else its score (scored in the background when it has none); blurring on or not
    std::unordered_map<std::wstring, int> nsfwMarks_; // (lower case)
    bool nsfwMarksLoaded_ = false;
    bool ShouldBlur(const std::wstring& path);
    void NoteFileDeleted(const std::wstring& path); // deleted from elsewhere in the app: the gallery lets go of it
    bool ShouldBlur(const ImageEntry& e);
    // What to draw for a picture's texture: itself, or (blurred) its soft copy - null while that is being made
    ID3D11ShaderResourceView* ShownSrv(ID3D11ShaderResourceView* srv, bool blur);
    bool showQualityBadges = true;  // quality chip on thumbnails that have been scored
    bool showNsfwDialog = false;
    bool nsfwOnlySelected = false;
    float nsfwThreshold = 0.5f;
    bool nsfwUseQuestionable = true;
    float nsfwQuestionableLow = 0.3f;
    std::string nsfwResultMessage;
    // Organize-folder settings (UTF-8; persisted between runs)
    std::string organizeSource;   // empty = current folder
    std::string organizeDest;     // empty = same as source
    std::string organizeExcludes; // comma-separated subfolder names to leave alone
    bool organizeVideos = true;
    bool organizeOtherFiles = true;
    bool organizeRemoveEmpty = true;
    bool organizeCopy = false;
    bool organizeKeepSubfolders = false;
    bool organizeDuplicates = true;
    std::string organizeCollection; // where "Add Folder to Collection..." sorts into

    // Vision prompt window (persisted settings + per-image results)
    VisionService vision;
    bool showVisionWindow = false;
    std::string visionModel;
    std::string visionKeepAlive = "10m"; // how long Ollama keeps the model loaded after use; "-1" = until unloaded
    std::string visionOutputDir;         // UTF-8
    bool visionAutoSave = true;
    bool visionUseTags = true;
    bool visionStopOnExit = true; // closing the app unloads models and stops Ollama (if it was used)
    std::string visionInstruction; // empty = DefaultVisionInstruction()
    enum class VisionMode { ImagePrompt, Tags, VideoScene };
    VisionMode visionMode = VisionMode::ImagePrompt;
    std::string visionTagsInstruction; // empty = DefaultTagsInstruction()
    VideoSceneOptions videoOptions;
    // Key / file-name suffix of the current mode ("", "tags", "video-10s-t").
    std::string VisionModeKey() const;
    std::wstring VisionTxtSuffix() const;
    static std::wstring VisionTextKey(const std::wstring& path, const std::string& modeKey);
    std::unordered_map<std::wstring, std::string> visionTexts; // latest prompt per image path (editable)
    std::string visionMessage;

    // Scrape dialog state
    bool showScrapeDialog = false;
    std::string scrapeFolder;
    bool scrapeRecursive = true;
    int scrapeFormat = 0; // 0 = CSV, 1 = JSON
    std::string scrapeResultMessage;

private:
    struct LoadedThumbResult {
        size_t index = 0;
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
        int helper = 0; // (HelperPixels of its thumbnail)
        ImageMetadata metadata;
        std::string searchTextLower;
    };

    // Cached across folder opens (and re-opens) for the lifetime of the app,
    // keyed by lowercased absolute path, and owns the thumbnail SRV. A cache
    // hit requires both last-write-time and size to still match, so an
    // edited/regenerated file is correctly re-decoded.
    struct CachedThumb {
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
        int helper = 0;
        ImageMetadata metadata;
        std::string searchTextLower;
        std::filesystem::file_time_type mtime{};
        uintmax_t size = 0;
    };

    std::thread loaderThread_;
    std::atomic<bool> cancelLoading_{false};
    std::atomic<bool> loaderActive_{false};
    std::atomic<int> loadedCount_{0};
    int totalCount_ = 0; // set once before the worker starts; read-only afterward

    std::mutex resultsMutex_;
    std::vector<LoadedThumbResult> pendingResults_;

    std::mutex cacheMutex_;
    std::unordered_map<std::wstring, CachedThumb> thumbCache_;

    DiskCache diskCache_;

    std::thread cleanThread_;
    std::atomic<bool> cleanRunning_{false};
    std::atomic<bool> cleanCancel_{false};
    std::atomic<int> cleanCurrent_{0};
    std::atomic<int> cleanTotal_{0};
    std::mutex cleanMutex_;
    std::string cleanFinishedMessage_;
    std::wstring cleanFinishedDir_;
    bool cleanFinished_ = false;

    std::thread renameThread_;
    std::atomic<bool> renameRunning_{false};
    std::atomic<bool> renameCancel_{false};
    std::atomic<int> renameCurrent_{0};
    std::atomic<int> renameTotal_{0};
    std::mutex renameMutex_;
    std::string renameFinishedMessage_;
    std::vector<std::pair<std::wstring, std::wstring>> renameFinishedMoves_; // as renamed: from -> to
    bool renameFinishedUndo_ = false;
    bool renameFinished_ = false;
    std::wstring latestRenameLog_;
    void RefreshRenameLog();

    FolderWatcher watcher_;
    std::wstring watchedFolder_;
    bool watchedRecursive_ = false;
    struct WatchPending {
        std::wstring path;
        unsigned long long tick = 0;
    };
    std::unordered_map<std::wstring, WatchPending> watchPending_; // lowercased path -> latest event
    std::unordered_map<std::wstring, int> pathIndex_;              // lowercased path -> index into entries
    int missingCount_ = 0;
    int hiddenCopies_ = 0;
    // Background folder scan
    std::thread listThread_;
    std::atomic<bool> listing_{false};
    std::atomic<bool> listCancel_{false};
    std::atomic<bool> listDone_{false};
    std::atomic<int> listingFound_{0};
    std::mutex listMutex_;
    std::vector<ImageEntry> listEntries_;
    int listHidden_ = 0;
    std::unordered_map<std::wstring, int> listPathIndex_, listCopyIndex_;
    std::wstring listSelectAfter_;
    bool entriesSortedByName_ = true;
    unsigned long long lastEpochTick_ = 0;
    bool epochPending_ = false;
    void CancelListing();
    void PumpFolderListing();
    void BumpEpochThrottled();
    // Vision library scan + preview loader
    bool libWanted_ = false;
    bool libDirty_ = false;
    unsigned long long libDirtyTick_ = 0;
    std::atomic<bool> libScanning_{false};
    std::thread libThread_;
    std::mutex libMutex_;
    std::vector<VisionLibItem> libPending_;
    std::vector<std::string> libPendingDirs_;
    std::string libPendingFolder_;
    bool libReady_ = false;
    std::mutex libThumbMutex_;
    std::unordered_map<std::wstring, LibThumb> libThumbs_;
    std::deque<std::wstring> libThumbQueue_;
    std::condition_variable libThumbCv_;
    std::thread libThumbThread_;
    bool libThumbStop_ = false;
    void StopLibraryThumbs();
    std::mutex describedMutex_;
    std::unordered_map<std::wstring, int> describedByCopy_;  // "name|size" of the output-folder copy -> mask
    std::unordered_map<std::wstring, int> describedPending_; // result of the background scan
    std::unordered_map<std::wstring, int> sessionMask_;      // lowercased source path -> mask, this session
    bool describedReady_ = false;
    bool describedDirty_ = true;
    unsigned long long describedDirtyTick_ = 0;
    std::thread describedThread_;
    std::atomic<bool> describedRunning_{false};
    int VisionMaskFor(const ImageEntry& e) const;
    std::mutex visionFolderMutex_;
    std::string visionFolderMessage_;
    std::unordered_map<std::wstring, int> copyIndex_; // "name|size|mtime" -> entry, to hide live-added copies
    std::thread visionFolderThread_;
    std::atomic<bool> visionFolderBusy_{false};
    std::atomic<bool> visionFolderCancel_{false};
    int visionBatchSaved_ = 0, visionBatchFailed_ = 0; // running totals for the message while a batch runs
    struct VisionPlan {
        std::string instruction;
        VisionService::DescribeOptions options;
        std::wstring saveDir;
    };
    VisionPlan MakeVisionPlan() const;
    int liveFollowIndex_ = -1;
    struct LiveJob {
        size_t index = 0;
        std::wstring path;
        std::filesystem::file_time_type mtime{};
        uintmax_t size = 0;
    };
    std::thread liveThread_;
    std::mutex liveMutex_;
    std::condition_variable liveCv_;
    std::deque<LiveJob> liveQueue_;
    bool liveStop_ = false;
    void EnqueueLive(LiveJob job);
    void StopLiveWorker();
    void RebuildPathIndex();
    std::vector<ID3D11ShaderResourceView*> retiredSrvs_; // replaced thumbnails, freed at exit
    void ProcessFile(size_t index, const std::wstring& path, std::filesystem::file_time_type mtime, uintmax_t size, bool persist);

    // Ratings: lowercased path -> stars.
    std::mutex ratingsMutex_;
    std::unordered_map<std::wstring, int> ratings_;
    bool ratingsDirty_ = false;
    unsigned long long ratingsChangedTick_ = 0;
    void LoadRatings();
    void SaveRatings();

    // Perceptual hashes (persisted), and the scan that fills/uses them.
    struct PHashRec {
        int64_t mtime = 0;
        uintmax_t size = 0;
        uint64_t hash = 0;
    };
    std::mutex phashMutex_;
    std::unordered_map<std::wstring, PHashRec> phashCache_;
    void LoadPHashCache();
    void SavePHashCache();
    std::thread similarThread_;
    std::atomic<bool> similarRunning_{false};
    std::atomic<bool> similarCancel_{false};
    std::atomic<int> similarCurrent_{0};
    std::atomic<int> similarTotal_{0};
    std::mutex similarMutex_;
    std::vector<std::vector<int>> similarPending_;
    std::string similarPendingMessage_;
    bool similarDone_ = false;
    std::wstring similarFolder_; // folder the pending result belongs to

    // Image-based people counts (persisted, invalidated when the model changes).
    struct PeopleRec {
        int64_t mtime = 0;
        uintmax_t size = 0;
        int count = 0;
    };
    PersonDetector personDetector_;
    std::mutex peopleCacheMutex_;
    std::unordered_map<std::wstring, PeopleRec> peopleCache_;
    std::string peopleModelId_;
    void LoadPeopleCache();
    void SavePeopleCache();
    struct PeopleResult {
        size_t index = 0;
        std::wstring path;
        int count = -1;
    };
    std::thread peopleThread_;
    std::atomic<bool> peopleRunning_{false};
    std::atomic<bool> peopleCancel_{false};
    std::atomic<int> peopleCurrent_{0};
    std::atomic<int> peopleTotal_{0};
    std::mutex peopleResultsMutex_;
    std::vector<PeopleResult> peoplePending_;
    std::string peopleFinishedMessage_;
    std::string peopleFinishedDevice_;
    bool peopleFinished_ = false;
    std::shared_future<void> diskCacheReady_;

    std::thread scrapeThread_;
    std::atomic<bool> scrapeRunning_{false};
    ScrapeProgress scrapeProgress_;
    std::mutex scrapeResultMutex_;
    std::string scrapeResultPending_;
    bool scrapeResultAvailable_ = false;
    ScrapeKind lastScrapeKind_ = ScrapeKind::None;

    struct CachedNsfw {
        int64_t mtime = 0;
        uintmax_t size = 0;
        float score = -1.0f;
    };
    struct NsfwScoreResult {
        size_t index = 0;
        std::wstring path; // guards against the entry list changing underneath
        float imageScore = -1.0f;
    };
    void LoadNsfwCache();
    // quality scores
    struct CachedQuality {
        int64_t mtime = 0;
        uintmax_t size = 0;
        float anime = -1, general = -1, prob = -1;
        int kind = -1;
        float kindConf = -1;
    };
    struct QualityResult {
        size_t index = 0;
        std::wstring path;
        float anime = -1, general = -1, prob = -1;
        int kind = -1;
        float kindConf = -1;
    };
    void LoadQualityCache();
    void SaveQualityCache();
    bool LookupQualityCache(ImageEntry& e);
    AestheticScorer aesthetic_;
    std::mutex qualityMutex_;
    std::unordered_map<std::wstring, CachedQuality> qualityCache_;
    std::string qualityModelId_;
    std::thread qualityThread_;
    std::atomic<bool> qualityRunning_{false}, qualityCancel_{false};
    std::atomic<int> qualityCurrent_{0}, qualityTotal_{0};
    std::mutex qualityResultsMutex_;
    std::vector<QualityResult> qualityPending_;
    std::string qualityMessage_;
    bool qualityFinished_ = false;
    void SaveNsfwCache();
    void LoadOrganizeSettings();
    void LoadVisionSettings();
    OrganizeHooks MakeOrganizeHooks();
    // Runs `job` on the NSFW worker thread; its result message/rescan flag
    // are delivered through PumpNsfwResults.
    void LaunchNsfwThread(std::function<OrganizeResult()> job);
    // Cached model score for a file, or negative if absent/stale.
    float LookupNsfwCache(const std::wstring& path, std::filesystem::file_time_type mtime, uintmax_t size);

    NsfwClassifier nsfwClassifier_;
    std::mutex nsfwCacheMutex_;
    std::unordered_map<std::wstring, CachedNsfw> nsfwCache_; // keyed by lowercased path
    std::string nsfwCacheModelId_;
    // Pictures outside the open folder with no score yet (LoRA comparisons, characters, ComfyUI results...): scored in the background, a
    // few at a time, and blurred until then. Before (user, 2026-10-23: "block nsfw clearly doesnt work") they were never scored, so never blurred.
    std::deque<std::wstring> nsfwAskQueue_;
    std::set<std::wstring> nsfwAsked_;      // (lower case) queued once
    std::set<std::wstring> nsfwUnscorable_; // (lower case, under nsfwCacheMutex_) not a picture the classifier can read: not blurred
    std::future<void> nsfwAskFut_;
    std::atomic<bool> nsfwAskStop_{false};
    void PumpNsfwAsks();

    std::thread nsfwThread_;
    std::atomic<bool> nsfwRunning_{false};
    OrganizeProgress nsfwProgress_;
    std::mutex nsfwResultsMutex_;
    std::vector<NsfwScoreResult> nsfwPendingScores_;
    std::string nsfwFinishedMessage_;
    bool nsfwFinished_ = false;
    bool nsfwNeedsRescan_ = false;
};
