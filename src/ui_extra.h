#pragma once
// Secondary windows and widgets: statistics, side-by-side compare, similar
// images, appearance settings and the star-rating control.
#include <functional>

#include "app_state.h"
#include "director.h"
#include "imgui.h"

namespace ui {

// Five clickable stars. Returns true when the user changed `rating` (0-5);
// clicking the current rating clears it.
bool RatingStars(const char* id, int& rating);

} // namespace ui

extern bool g_boxSelecting; // a page's grid just began a selection box: that drag is not a move of the page

// Explorer-style folder tree (View > Folder tree): narrows the gallery to a subfolder, or opens another folder.
void DrawFolderPane(AppState& app, float height);
void DrawLibraryFolderPane(AppState& app, float height); // folders inside the Vision output folder
void DrawStatsWindow(AppState& app);
// Over a blurred NSFW picture (a..b): a "Hidden: NSFW - Show it" pill; a click on it reveals that picture for this session.
void DrawNsfwCover(AppState& app, const std::wstring& path, ImVec2 a, ImVec2 b);
// Over a picture drawn in a..b: the NSFW cover when it is blurred ("Show it"), or "Blur it again" (on hover) when it was shown on purpose
void DrawNsfwOverlay(AppState& app, const std::wstring& path, ImVec2 a, ImVec2 b);
// In a picture's right-click menu: Mark as NSFW / not NSFW (your call over the classifier's), Blur it again
void NsfwMarkMenu(AppState& app, const std::wstring& path);
// Under a job ComfyUI is generating: its step, time a step and time left (from the log of a ComfyUI this app started), and a warning when the
// PC runs out of memory or the steps suddenly get much slower.
void DrawComfyJobProgress(const ComfyActivity& a);
void EnsureComfyService(AppState& app);             // ComfyUI's settings read and its service made (once)
void OpenSetup(int step = 0);                       // setup_ui.cpp: the first-run setup window (Help > Set up Try2Create)
void DrawFirstRunSetup(AppState& app);                      // once a frame: opens itself on a first start that has nothing to make with
bool SetupDownloading();                            // its model downloads are going
bool SetupDownloadProgress(int& doneGb, int& totalGb); // GB done and in all, while they go
void StopSetupDownloads();
void SetupDownloadPack(const char* name);           // a pack's missing files ("3D"...), from the page that needs them
std::string SetupDownloadLine(float* frac);         // what they are at, for the status bar ("" = none)
bool DrawComfyTracker(AppState& app, float width); // the status bar's ComfyUI jobs (any page): false when there are none
void DrawPresetArt(ImDrawList* dl, int k, ImVec2 p, float w, float h, float t); // preset_art.cpp: the Video page's motion preset k drawn (t < 0: still)
int ComfyJobTab(const std::string& label);         // the page a ComfyUI job belongs on
// Above a list of results (Edits, Upscale, Pixel art): "Take off missing" and "Clear the list..." (asks first). Returns 1 = take off the
// entries with no picture left to show (moved, deleted or failed), 2 = clear every finished entry, 0 = nothing. Files are never deleted.
int DrawListCleanup(const char* id, bool any);
bool FileStillThere(const std::wstring& path);
void DrawCompareWindow(AppState& app);
void DrawSimilarWindow(AppState& app);
void DrawPeopleDetectWindow(AppState& app);
// The "Vision library" tab: browse the Vision output folder, reading each image's prompt, tags and video prompts.
void DrawVisionLibrary(AppState& app, float height);
// The "Workflows" tab: ComfyUI node viewer and the library of extracted workflows.
void DrawWorkflowTab(AppState& app, float height);
// The "Vision board" tab: library pictures become cards on a canvas, linked by relations, and turn into one scene prompt.
void DrawBoardTab(AppState& app, float height);
// Queues a prompt in a running ComfyUI (the workflow chosen in the ComfyUI window). With useDefaults the negative prompt and the picture
// are only sent when the window's options say so; otherwise exactly what is passed is sent.
// `seconds` > 0 marks a video prompt: its picture is sent as the first frame and a video workflow's clip length is set from it.
// `times` > 0 makes that many pictures from it (0 = the setting); jobs sent together share a `batch` number. False = nothing was sent.
bool SendToComfy(AppState& app, const std::string& prompt, const std::string& negative, const std::wstring& picture, const std::string& label,
                 bool useDefaults = true, double seconds = 0, int times = 0, int batch = 0);
// ---- video with sound: MiniMax H3 (video_ui.cpp, the Video page)
struct H3Options {
    int quality = 1;      // 0 fast: Wuli Turbo, 4 steps (fine detail can morph); 1 balanced: Alibaba's 8-step accelerator (the default); 2 best: the model alone, 20 steps;
                          // 3 presenter: TaoMate's 3-step LoRA (a person talking to the camera)
    int res = 2;          // 0 480p, 1 640p, 2 768p (what it was trained at), 3 1080p
    int shape = 0;        // 0 like the start picture (landscape without one), 1 landscape 16:9, 2 portrait 9:16, 3 square
    double seconds = 5;   // 3-15
    int times = 1;
    std::wstring last;    // the last frame (optional)
    bool veda = false;    // Veda sparse attention's node goes in (set as it is sent: the page's switch, when ComfyUI has the node and predictor)
    int parts = 0;        // H3 Continuum: made as this many parts that continue each other, partSeconds each (0 = by the length: over 15 s)
    double partSeconds = 0;
    std::vector<LoraEntry> loras; // put on the model ("" = your MiniMax H3 LoRAs from the settings)
    bool ownLoras = false;        // loras is the whole list, even empty (the LoRA browser's with / without)
    bool fixedSeed = false;       // the workflow's own seed, not a new one (the LoRA browser compares like with like)
    // LTX-2.5 only (NVAI-open's ways, MIT): 0 the two-stage way, 1 DFR (Lightricks' pixel-spatial IC-LoRA, three stages), 2 DFR at 48 fps
    int ltxDetail = 0;
    bool ltxLoop = false;        // the start picture is the last frame too, with a still camera
    bool ltxCinemagraph = false; // Lightricks' Cinemagraph LoRA: one thing moves (with the loop)
    double ltxSlow = 1.0;        // Slow-Motion-Control: 0.5 = 2x slower ... 0.1 = 10x (1 = off)
};
bool H3LongReady(); // H3 Continuum is there (videos over 15 s, and parts that continue each other)
H3Options& VideoOptions(); // the Video page's settings: every H3 button uses them
// For the story maker: the Video page's video-prompt writer (blocks: call it on a worker thread; `note` = why it fell back, "" = written),
// its time estimate and size for these settings, and its player.
std::string WriteVideoPromptFor(const std::wstring& first, const std::string& idea, const std::string& context, const H3Options& o, const std::string& comfyUrl,
                                std::string* note);
double EstimateH3Seconds(const H3Options& o, int w, int h);
void H3SizeFor(const H3Options& o, int srcW, int srcH, int& w, int& h);
const char* H3QualityName(int quality);
void PlayInVideoPage(AppState& app, const std::wstring& path, bool autoplay = true); // autoplay false: shown paused on its first frame
// The Video page's "Make a story" mode (story_ui.cpp): draws its form (ending the form's child window) and footer; PumpStory runs each frame.
void DrawStoryMode(AppState& app, float footerH);
void PumpStory(AppState& app);
// The Video page's "Make a music video" mode (music_video.cpp): a song, your characters, one connected video that sings and acts it out
void DrawMusicVideoMode(AppState& app, float footerH);
// Make a song, on that page: the writer and MiniMax Music make one from what is on it; ...Take gives the finished file once, ...Failed why it could not
void MusicMakeForVideo(AppState& app, const std::string& idea, const std::string& context, const std::vector<std::wstring>& photos, double seconds, bool instrumental);
std::string MusicForVideoStatus();
std::wstring MusicForVideoTake();
void MusicVideoSongMoved(const std::wstring& from, const std::wstring& to); // a song was renamed on the Sound page: the music video keeps it
std::string MusicForVideoFailed();
void DrawMusicArt(ImDrawList* dl, int art, ImVec2 p, float w, float h, float t); // the music video's cards (preset_art.cpp), laid out on 100 x 50; t: seconds since hovered (< 0: still)
void PumpMusicVideo(AppState& app);
void OpenMusicVideo(AppState& app, const std::wstring& song); // the Video page in that mode, with this song
void SetVideoMode(int mode);                                   // the Video page's mode (0 animate, 1 star, 2 story, 3 music video)
struct MusicSongInfo {
    std::wstring path;
    std::string title;
    double seconds = 0;
};
std::vector<MusicSongInfo> MusicSongs(); // the Music page's songs, newest first (empty until that page has looked)
// A video with sound from MiniMax H3 at those settings: `first` is its first frame (none: from the prompt alone). False = nothing was sent.
bool SendH3(AppState& app, const std::wstring& first, const std::string& prompt, const H3Options& o, const std::string& label);
// The quick buttons (Vision library, send editor, make bar): the Video page's quality and size, this picture and length.
bool SendH3Turbo(AppState& app, const std::wstring& picture, const std::string& prompt, double seconds, const std::string& label, int times = 1);
std::wstring H3TurboWorkflow(bool fromPicture); // tools\h3turbo's ready-made workflow ("" = not next to the program)
// Runs this API workflow file as it is (no ComfyUI tab choices), with the prompt, the clip length, `picture` in its LoadImage and nodePictures in theirs.
bool SendH3Workflow(AppState& app, const std::wstring& workflow, const std::wstring& picture, const std::vector<std::pair<std::string, std::wstring>>& nodePictures,
                    const std::string& prompt, double seconds, int times, const std::string& label, bool fixedSeed = false);
// The Video page with this start picture. `picturePrompt` (the picture's own prompt: what it shows) only helps the writer; the video prompt is written fresh.
// `videoPrompt`: a ready video prompt (a Vision video scene) used as it is.
void OpenVideoPage(AppState& app, const std::wstring& first, const std::string& picturePrompt, const std::string& videoPrompt = std::string());
// Writes a video prompt for the picture and the idea (the prompt writer looks at it), then makes the video: the make bar's Video mode.
void SendH3Auto(AppState& app, const std::wstring& first, const std::string& idea, double seconds, const std::string& label, int times = 1,
                const std::vector<LoraEntry>& loras = {});
void VideoPagePressMake();                  // Make on the Video page, as if clicked (when the page is drawn next)
void PumpComfyResults(AppState& app);       // each frame: the ComfyUI tab's video player pauses when you leave the tab
void PumpVideo(AppState& app);              // each frame: the prompt writer's queue, the page's player
bool MusicPlaying();                        // a song is playing on the Music page (keep drawing: its lyrics and bar move)
bool VideoPageAnimating(AppState& app);     // the page's player or writer is busy (keep drawing)
void DrawVideoTab(AppState& app, float height);
std::string VideoTabLabel(AppState& app); // "Video (1)" while something is being made
std::string ComfyWorkflowLabel(AppState& app); // "ComfyUI's last run" / the saved workflow's name
void ComfyWorkflowMenu(AppState& app); // menu items: which ComfyUI workflow prompts run in
void PumpComfy(AppState& app);       // each frame: ComfyUI's messages into the status line
void DrawComfyTab(AppState& app, float height); // the ComfyUI tab
// A window to change a prompt before sending it (from anywhere); the original is never touched. Drawn every frame by the main loop.
void OpenSendEditor(AppState& app, const std::string& prompt, const std::string& negative, const std::wstring& picture, const std::string& label, double seconds = 0);
void DrawSendEditor(AppState& app);
// Edit pictures with Qwen Image 2.1 Edit (built in): the first picture is edited, up to 9 more are references (<image1>, <image2>...).
// `ctx` (optional) says what the pictures are and which Vision board they came from, so the Edits tab can show the board and swap characters.
struct EditContext {
    std::vector<std::string> roles; // what each picture is ("Knight", "the setting"), same order as the pictures
    bool hasBoard = false;
    Board board;
    int scene = 0;
    std::string prompt;             // extra direction (style, camera notes) given with the scene
    std::string finalPrompt;        // the board's written final prompt, when there is a current one (can be included in the instruction)
    std::string looksText;          // the directions (outfit, pose, expression) that are in the instruction now, so they can be swapped
    int canvas = 0;                 // 0 = the first picture is edited (its size and frame); 1 landscape, 2 square, 3 portrait = a NEW canvas, the pictures are only references
    bool pixelArt = false;          // pixel art mode: the result also becomes pixel art (Pixel art tab), collected under pixelGroup
    std::string pixelGroup;         // "Clef mode", "Vision board"... (empty: "Edits")
    bool fillMask = false;          // a painted area (Edits > Paint and fill): `mask` (white = redrawn) goes with the picture, FLUX 3 fills only there
    std::wstring mask;
    std::wstring grey;              // ...and the picture with that area flat grey (what FLUX 3 is shown)
};
bool SendPictureEdit(AppState& app, const std::vector<std::wstring>& images, const std::string& instruction, const std::string& label, int times = 1,
                     const EditContext* ctx = nullptr);
void OpenPictureEditor(AppState& app, const std::vector<std::wstring>& images, const std::string& instruction = std::string(), const std::string& label = std::string(),
                       const EditContext* ctx = nullptr);
// The Edits tab: every picture edit sent to Qwen Image 2.1 Edit, its result, what it was made from (and the Vision board), swap on the fly.
int RegisterEdit(AppState& app, int activityId, const std::string& label, const std::string& instruction, const std::vector<std::wstring>& images, const EditContext* ctx, int times = 1);
// How long a picture edit takes on THIS computer: measured from your own runs (median of the last few), a rough guess before there are any.
void RecordEditRun(int steps, int res, int nImages, int canvas, int times, double generatingSeconds);
double EstimateEditSeconds(int steps, int res, int nImages, int canvas);
std::string EditEstimateSource(); // "from your last 5 runs" / "a rough guess: no run has been timed on this PC yet"
std::string FormatEditTime(double seconds);
void DrawEditsTab(AppState& app, float height);
void DrawAppsBody(AppState& app, float height); // Edits > Apps (apps_ui.cpp): one-click picture recipes (relight, new angle, expand, headshot...)
void DrawPaintFillBody(AppState& app, float height); // Edits > Paint and fill (paint_fill.cpp): a brushed area redrawn by FLUX 3
// The Video page's tools for finished videos (video_tools.cpp): a transition between two, the background taken out, an edit cut from several
void OpenTransition(const std::wstring& a, const std::wstring& b);
void OpenBackgroundRemoval(const std::wstring& clip);
void OpenAutoEdit(const std::vector<std::wstring>& clips);
void DrawVideoTools(AppState& app, const std::string& sam3); // every frame on the Video page: its windows and its work
std::string VideoToolsStatus(); // what it is doing ("" = nothing)
bool VideoToolsMadeVideo();     // a new video is there since asked last (the list looks again)
// The Upscale tab: make pictures bigger / make their faces better (photo or anime), with ComfyUI (an upscale model, then a Qwen redraw pass).
void DrawUpscaleTab(AppState& app, float height);
// Clef mode (the fun mode): Clef, or plain chance, keeps inventing scenarios for the saved characters and sending them to Qwen Image 2.1 Edit.
void DrawClefTab(AppState& app, float height);
std::string ClefTabLabel(AppState& app);
void ClefShutdown();
void AddToClefCast(AppState& app, const std::vector<std::wstring>& paths); // pictures join the Clef mode cast (from any picture menu)
int LastEditActivity(); // the activity id of the edit SendPictureEdit sent last (0 = none)
std::string UpscaleTabLabel(AppState& app);
void OpenUpscaler(AppState& app, const std::vector<std::wstring>& paths); // shows the tab with these pictures ready to upscale
int SendUpscale(AppState& app, ComfyJob job);                             // the id of its entry in the activity list (0 = not sent)
std::string EditsTabLabel(AppState& app);
// The Pixel art tab (Retro Diffusion through ComfyUI): pixel art from a prompt, a picture turned into pixel art, whole sets (characters, items, tiles...) planned by the prompt writer.
void DrawPixelTab(AppState& app, float height);
std::string PixelTabLabel(AppState& app);
void OpenPixelArt(AppState& app, const std::vector<std::wstring>& paths);
void QuickPixelArt(AppState& app, const std::vector<std::wstring>& paths); // made at once with the Convert settings (shows in the Pixel art tab)
void PixelArtQuickContents(AppState& app, const std::vector<std::wstring>& paths); // the small "Pixel art" panel, inside a popup or a menu
void PixelArtButton(AppState& app, const std::vector<std::wstring>& paths, const char* popupId); // a "Pixel art..." button opening that panel
void DrawPixelArtNotices(AppState& app);
// Pixel art mode (Clef mode, the Vision board, the picture editor): when ComfyUI finishes job `activity`, its pictures become pixel art with the Pixel art
// tab's settings (a width that suits each picture), collected in the set `group` of the Pixel art tab.
void PixelArtAfter(AppState& app, int activity, const std::string& group, const std::string& label, bool notify);
bool PixelArtThumb(AppState& app, int activity, float size); // the pixel art made from that job, crisp (click: open it); false while there is none
std::string PixelArtLookClause();
int PixelCutoutPicture(AppState& app, const std::wstring& src);              // SAM 3 cuts the main subjects out of a picture: the ComfyUI job (0 = not sent)
int PixelCutoutResult(AppState& app, int activity, std::wstring& cutPng);    // 0 still working, 1 done (cutPng: the cut-out, cropped, with alpha), -1 failed
// Design with PhotoCraft: titles, captions, memes, thumbnails, posters and scenes on a picture (tools\photocraft), planned by the prompt writer.
void OpenDesigner(AppState& app, const std::vector<std::wstring>& paths);   // the first picture is designed on; the others are placed into it (cut out first)
void DrawDesigner(AppState& app);                                           // once a frame (its jobs)
void DrawDesignerBody(AppState& app, float height);                        // Edits > Text over a picture
void SetEditsMode(int mode); // the Edits page: 0 Qwen picture edits, 1 MOSS designs & box edits, 2 text over a picture (PhotoCraft), 3 upscale,
                             // 4 apps, 5 videos (Make it crisp), 6 the start screen, 7 paint and fill
int EditsMode();
bool EditsHomeAnimating();   // a card on the Edits start screen is playing its drawing (keep drawing frames)
void DrawVideoEditsBody(AppState& app, float height); // video_ui.cpp: Edits > Videos (the player with Make it crisp, the videos)
void DrawEditsArt(ImDrawList* dl, int mode, ImVec2 p, float w, float h, float t); // preset_art.cpp: the Edits start screen's drawings
void DesignerTestHook(AppState& app);                                       // MV_DESIGN_TEST=<picture>|<request>[|<picture to place>]                           // asks an edit or a prompt for a pixel-art look
void PixelArtModeOptions(const char* id, bool& look);       // the options under a "Pixel art mode" checkbox (how, see-through, the look)                                   // the corner cards (sent, ready, failed), once a frame
int SendPixel(AppState& app, ComfyJob job);
int SendSheetEdit(AppState& app, ComfyJob job);
int SendComfyJob(AppState& app, ComfyJob job); // any job (a complete workflow in job.apiWorkflow): the id of its activity
// The Music tab (music_ui.cpp): songs and soundtracks with MiniMax Music 3
void DrawMusicTab(AppState& app, float height);
void PumpMusic(AppState& app);
void OpenMusicTab(AppState& app, const std::string& idea, double seconds, bool instrumental); // the tab, with this idea filled in
// A song about a picture: the Music tab with it as the theme picture; the writer looks at it (`context`: what it shows, e.g. its prompt) and
// writes the music, lyrics and title. Nothing is made until Make is pressed.
void MusicFromPicture(AppState& app, const std::wstring& picture, const std::string& context);
// A video to post a sound with: the picture, still, for the whole of it, with its .srt (timed lyrics or lines) burned in as subtitles.
// tall: 1080 x 1920 (the picture fills it), else 1080 x 1080. Blocks (a worker thread): the video's path, or "error: ...".
std::string MakeAudioPostVideo(const std::wstring& audio, const std::wstring& picture, const std::wstring& out, bool tall);
// The Drama tab (drama_ui.cpp): stories, comedy and action scenes with many speakers, spoken by Fish Audio Drama 3
void DrawDramaTab(AppState& app, float height);
void PumpDrama(AppState& app);
bool DramaPlaying(); // a scene is playing on the Drama page (keep drawing: its lines and bar move)
// A chosen picture, big: the gallery shows it when it is in the folder loaded there, otherwise Windows opens it (Photos)
void ViewPictureLarge(AppState& app, const std::wstring& path);
void SetStoryMusic(const std::wstring& song);   // the story's soundtrack (story_ui.cpp)
std::string StoryMusicIdea(double* seconds);     // what music would suit the story, and its length
bool PixelArtAnimating();                                                  // the Pixel art tab is playing an animation (keep drawing frames)                           // a Qwen sprite-sheet edit (Qwen needs the GPU: the LLMs this app loaded give it back)                               // the id of its entry in the activity list (0 = not sent)
void PumpPixelArt(AppState& app);                                        // each frame: results are finished (and tile seam passes sent) whichever tab is showing
// The Vision board turned into a picture-edit request: the pictures (setting first, characters, items...), what each is, and the instruction.
struct BoardEditOptions {
    int styleMode = 0;                                   // 0 as asked, 1 nano banana, 2 fit everything to the scene
    DirectorFlags flags;                                 // which of outfit / pose / expression get a concrete direction
    std::string place;                                   // a place in words, used when the board has no setting picture (Clef mode)
    const std::vector<std::pair<std::wstring, CharLook>>* fixedLooks = nullptr; // directions that were decided already, by character picture (Clef mode)
    std::string looksOut;                                // out: the directions that went into the instruction (so a window can swap them)
};
// Which Qwen model picture edits load: the new NoctQ one, the old Qwen Image 2.1 one, or any other file. True when the choice changed (and was saved).
bool DrawEditModelPicker(ComfySettings& settings, const char* id);
bool DrawEditorChoice(ComfySettings& settings); // the Editor combo: Qwen Image 2.1 Edit or FLUX 3 Image Klein (true = changed)
bool Flux3EditReady();                          // FLUX 3 Image Klein is in ComfyUI's diffusion_models
// The editor of a list of LoRAs (on / file / strength / remove, add from ComfyUI's list or a file). True when it changed (and was saved).
bool DrawLoraList(ComfySettings& settings, std::vector<LoraEntry>& list, const char* id);
DirectorFlags DirectorFlagsFor(const ComfySettings& settings, int styleMode); // the switches of the Options + the style
std::string BoardEditRequest(const Board& board, int scene, const std::string& extra, std::vector<std::wstring>& images, std::vector<std::string>& roles, int& canvas, BoardEditOptions& options);
// The directions for the board's characters alone (new random picks), for the window that lets you shuffle them. `images` is the order of the pictures in the instruction.
std::string BoardLooksClause(const Board& board, int scene, const std::vector<std::wstring>& images, const std::string& extra, const BoardEditOptions& options);
// Clef mode: a scenario turned into a board of its own, ready for BoardEditRequest. `pool` characters are saved characters (picture, name, description).
struct CastMember {
    std::wstring image;
    std::string name, description;
};
Board BoardFromScenario(const std::vector<CastMember>& cast, const Scenario& scenario, bool placeCard = false); // placeCard: the place as a setting card (text prompts have no setting picture)
void PumpClefMode(AppState& app); // each frame: Clef mode keeps going whichever tab is showing
// A multi-line text that is always fully readable: long text is shown WRAPPED (a plain ImGui text box cuts long lines off on the right); press "Edit the
// text" (or double click it) to change it. Short text goes straight to the editor. True when the text changed.
bool WrapEdit(const char* id, std::string& text, float height);
// How freely the characters may be changed for a scene: 0 = not at all, 1 = "nano banana" style, 2 = fit everything to the scene (face, body and character style stay).
std::string EditStyleClause(int mode);
const char* EditStyleName(int mode);
std::string FinalPromptClause(const std::string& finalPrompt); // "Scene description (the written final prompt of the board): ..."
Board RecastBoard(const Board& original, const std::vector<std::pair<std::wstring, std::wstring>>& map); // the board with other characters in place
void DrawPictureEditor(AppState& app);
void DrawComfyStatusDot(AppState& app);          // toolbar: is ComfyUI running (click: its tab)
// The Characters tab (saved characters, swap a character into a picture, fun remixes).
void DrawCharactersTab(AppState& app, float height);
void PumpCharacterResults(AppState& app);                          // each frame: the model's answers for the Characters tab
void AddSavedCharacter(AppState& app, const std::wstring& path);  // keeps a picture as a character (and has it described)
void ShowSavedCharacter(AppState& app, const std::wstring& path); // the Characters tab, with this saved character picked
// Create a character (character_creator.cpp, in the Characters tab): built from looks or your photo, drawn by Qwen, saved as a character.
void OpenCharacterCreator(AppState& app);
bool CharacterCreatorShown();
void ShowCharacterCreator(bool on);
void DrawCharacterCreator(AppState& app, float height);
void PumpCharacterCreator(AppState& app); // each frame: a character drawn becomes a saved character
bool CharacterCreatorBusy();
void StarInVideo(AppState& app, const std::wstring& who); // the Video page, Star in a video, with this person / character
void DrawClearCharacters(const char* id);
void MakeCharacterSheetFor(AppState& app, const std::wstring& image); // a character sheet of this saved character (kept in its profile when done)
// Backstories and names (character_story.cpp): the writer looks at the picture and writes who they are; a placeholder name ("Lyra 2", "Blonde
// Bird", a file name) is replaced by one it invents (forceRename: always a new one)
void QueueCharacterStory(AppState& app, const std::wstring& image, bool forceRename = false);
bool CharacterStoryPending(const std::wstring& image);
int CharacterStoriesQueued();
bool CharacterNameIsPlaceholder(const CharacterProfile& p);
void PumpCharacterStories(AppState& app); // each frame
// Characters out of a picture: the creator's third way (the writer finds them; Qwen Image Edit draws each alone)
void OpenCharacterExtract(AppState& app, const std::wstring& picture);
void DrawExtractSection(AppState& app);
int ExtractFoundOn();
void MakeExtracted(AppState& app);
void SetCharacterCreatorMode(int mode); // 0 design one, 1 invent a cast, 2 out of a picture
bool CharacterSheetPending(const std::wstring& image);
// A character's 360-degree view (video_ui.cpp: H3 + Pablo Dawson's 360 Orbit LoRA, kept in its profile when done; orbit_view.cpp shows it)
void MakeCharacterOrbit(AppState& app, const std::wstring& image);
bool CharacterOrbitPending(const std::wstring& image);
bool DrawOrbitSetup(AppState& app);        // the LoRA's download button while it is missing; true when it is there
void PumpCharacterOrbits(AppState& app);
// Layout designs (layout_ui.cpp, on the Edits page): designs with exact words from boxes (MOSS-Image layouts). With a picture: change the boxed
// words / things in it. Open: the Edits page with it shown.
void OpenLayoutStudio(AppState& app, const std::wstring& picture = {});
void PumpLayoutStudio(AppState& app);                    // each frame (its jobs finish while another page is open)
void DrawLayoutStudioBody(AppState& app, float height);  // the Edits page's "Layout designs"
bool LayoutStudioOpen();                                  // the Edits page shows Layout designs
void ShowLayoutStudio(bool on);
bool MossReady(AppState& app);  // ComfyUI has MOSS-Image's files (asks it now and then)
// a plain MOSS picture (8 steps): its activity id. savePrefix "" = Generate/moss; loras go on its model too.
int SendMossPrompt(AppState& app, const std::string& prompt, int w, int h, int times, const std::string& savePrefix = std::string(),
                   const std::vector<LoraEntry>& loras = {}, long long seed = -1);
// Generate's pictures as calls of their own (the LoRA browser): Krea 2 at w x h, Qwen Image 2.1 in the shape ax:ay (about a megapixel); only
// these LoRAs (none of the settings' lists); seed -1 = a new one. The activity id (0 = not sent).
int SendKreaPicture(AppState& app, const std::string& prompt, int w, int h, int times, const std::string& savePrefix, const std::vector<LoraEntry>& loras,
                    long long seed = -1);
int SendQwenPicture(AppState& app, const std::string& prompt, int ax, int ay, int times, const std::string& savePrefix, const std::vector<LoraEntry>& loras,
                    long long seed = -1);
// The Generate page's prompt box (it wraps its lines; the line breaks are only shown) and its text as sent (the breaks as spaces)
bool PromptBox(const char* id, std::string& text, ImVec2 size);
std::string PromptText(const std::string& boxText);
// A short, quick MiniMax H3 clip from text with only these LoRAs and the workflow's own seed (the LoRA browser): the activity id (0 = not sent)
int SendH3LoraPreview(AppState& app, const std::string& prompt, const std::vector<LoraEntry>& loras);
// The LoRA browser (lora_browser.cpp): every LoRA with what it does, shown as the same picture without it and at a few strengths
void OpenLoraBrowser(const char* model = nullptr);
void DrawLoraBrowser(AppState& app); // every frame (its queues; OpenLoraBrowser goes to the LoRAs page)
void DrawLoraPage(AppState& app, float height); // the LoRAs page (Browse): what each one does, and mixes tested on a picture
void TestLoraMixesOn(AppState& app, const std::wstring& picture); // that picture on the LoRAs page's Test mixes
void ShowLoraInBrowser(const std::string& lora, const char* model); // the LoRAs page with that one shown
// A LoRA's showcase picture fitted into w x h (its comparison, at the strength picked for it), or a box with its name when it has none; true when clicked
bool DrawLoraTile(AppState& app, const std::string& lora, float w, float h);
// A LoRA strength: 0 to 2 with a mark every 0.25 (it snaps to them while dragged), mouse wheel +-0.25, right-click 1, Ctrl+click types (-1 to 2)
bool StrengthSlider(const char* id, double& strength, float width);
// A LoRA's trigger word(s) for a prompt ("" = none known: from its file, else Civitai, looked up once in the background, else its title)
std::string LoraTrigger(const std::string& lora);
// The prompt with the trigger words of the LoRAs that are on (each part of a long video gets them; those already in it are not added again)
std::string WithLoraTriggers(const AppState& app, const std::string& prompt, const std::vector<LoraEntry>& loras);
// ComfyUI's LoRAs with their models (lora_sort.cpp), asked now and then; the file behind one; a file's header metadata ("" = none)
struct LoraOffer {
    std::string name, model;
};
const std::vector<LoraOffer>& LoraOffers(AppState& app);
void RefreshLoraOffers();
std::wstring LoraLocalFile(const std::string& comfyName);
std::string LoraHeaderMetadata(const std::wstring& file);
// ---- LoRAs by model (lora_sort.cpp)
// The model a LoRA is for, read from its file (training tag, else the shape of its weights): "Krea 2", "Qwen Image 2.1", "MOSS-Image",
// "MiniMax H3", "Flux", "SDXL"... ("" = not known). By ComfyUI's name of it (with any subfolder). Reads the file: not every frame.
std::string LoraModelOf(const std::string& comfyName);
// One model's LoRAs on one line: on / off, strength, take out, and "+ LoRA" listing ComfyUI's LoRAs made for `model` (as LoraModelOf names
// it; "" = all). Saves the settings when changed (true).
bool DrawModelLoras(AppState& app, std::vector<LoraEntry>& list, const char* model, const char* id, const char* label = "LoRAs:");
void OpenLoraSorter();             // Tools > Sort LoRAs by model
void DrawLoraSorter(AppState& app); // every frame (its window, while open)
void OpenLayoutWithIdea(AppState& app, const std::string& idea); // Edits > Layout designs, the writer planning this idea
// Generate (generate_ui.cpp, the top of the page that was "ComfyUI")
void DrawGenerateComposer(AppState& app);
void DrawGenerateMany(AppState& app); // Generate > Many at once: a theme becomes many pictures
void DrawPeopleComposer(AppState& app); // Edits > People & outfits: people, outfits and poses from pictures (its Make)
void GenerateUseLoras(AppState& app, const ImageMetadata& meta);  // the picture's LoRAs on Generate's list, for the model that made it
std::vector<LoraEntry> LorasInPicture(const ImageMetadata& meta, std::string* modelFiles = nullptr);
std::string ResolveLora(const std::string& name);                 // this ComfyUI's name for it ("" = not installed)
std::string LoraDisplayName(const std::string& name);             // without folder and extension
void ResultsLetGo(const std::wstring& path);                      // the viewer closes this video (it is being deleted)
void VideoDeleteMade(AppState& app, const std::wstring& path);    // a made video to the Recycle Bin, with its side files
int CharacterCreatorMode();
void DrawThreeDTab(AppState& app, float height);                  // the 3D page: a picture becomes a 3D object (Idol)
void ThreeDTakePicture(const std::wstring& path);                 // its picture (dropped on the page)
void OpenThreeD(AppState& app, const std::wstring& picture);      // that page with this picture, ready to make
bool ThreeDAnimating();                                           // it wants drawing every frame
void ThreeDRescan();                                              // look through the 3D objects again
void VideosRescan();                                              // and the VIDEOS list
std::vector<std::wstring> VideoFilesOf(const std::wstring& video); // a video and the files kept beside it
void VideoForget(const std::wstring& video);                      // off the VIDEOS list, its player lets go
// Pictures dropped on a page (from the gallery, the Vision library or Explorer): each page puts them where it takes pictures
bool GenerateTakePictures(const std::vector<std::wstring>& paths, bool peoplePage); // Generate: people (a video's start); People & outfits: pictures to use
bool VideoTakePictures(const std::vector<std::wstring>& paths);                     // Animate a picture: the start (a second: the end); false in other modes
bool AppsTakePictures(const std::vector<std::wstring>& paths);                      // Edits > One-click apps: its picture(s)
void PaintFillTakePicture(AppState& app, const std::wstring& path);                  // Edits > Paint and fill: the picture
void GenerateShowSent(const std::string& prompt); // a prompt sent to ComfyUI from elsewhere: Generate's box shows it (pictures, people, pose cleared)
// A making page's two columns: `left` (what to make) above what ComfyUI made, on the left; the picture, big, on the right
void DrawMakerColumns(AppState& app, float height, const char* id, void (*left)(AppState&));
int& GenerateSubTab();                // Generate's sub-tab: 0 Make, 1 Many at once, 2 Sent, 3 Settings
void GenerateWithPrompt(AppState& app, const std::string& prompt, const std::vector<std::wstring>& refs = {});
bool GenerateAgain(AppState& app, const std::string& prompt, const std::string& label, int times); // a result again, by the maker that made it
std::string GenerateKreaModel(); // the Krea 2 model ComfyUI has ("" = none)
std::string MoodyMossModel();    // layout_ui.cpp: a Moody MOSS (moodyMossMix) file in diffusion_models ("" = none)
int SendMoodyMossPrompt(AppState& app, const std::string& prompt, int w, int h, int times, const std::vector<LoraEntry>& loras, long long seed = -1);
std::string MossDepthControlLora();
std::string QwenDepthControl();     // Qwen Image 2.1's Fun ControlNet Union in models/model_patches ("" = none)
std::string KreaDepthControlLora(); // Krea 2's depth control LoRA in models/loras ("" = none)
// Qwen 2.1 / Krea 2 drawing on a picture's layout (its MoGe depth): a photo, a video frame, or a pose drawing (drawn = the control lets go sooner)
int SendQwenDepthPrompt(AppState& app, const std::string& prompt, const std::wstring& layout, bool drawn, int times, const std::vector<LoraEntry>& loras,
                        long long seed = -1);
int SendKreaDepthPrompt(AppState& app, const std::string& prompt, const std::wstring& layout, bool drawn, int times, const std::vector<LoraEntry>& loras,
                        long long seed = -1); // the MOSS color-depth control LoRA in loras ("" = none)
// MOSS (moody: Moody MOSS) drawing on the layout (depth) of `layout`; drawn = it is a drawn mannequin (the control ends half-way)
int SendMossLayoutPrompt(AppState& app, bool moody, const std::string& prompt, const std::wstring& layout, bool drawn, int times,
                         const std::vector<LoraEntry>& loras, long long seed = -1);
// The picture with a labelled grid every 10% (PhotoCraft), in w x h pixels: an LLM can tell where things are on it ("" = PhotoCraft missing)
std::wstring DesignGridPicture(const std::wstring& picture, int w, int h);   // each frame: a finished view goes into its character's profile
bool DrawOrbitView(AppState& app, const std::wstring& video, ImVec2 size); // drag to turn it; it turns slowly by itself
bool OrbitViewAnimating();                 // a view was on screen just now (keep redrawing)
// Does this character's sheet show what its picture cannot (its back looks unlike its front)? Only then is it used (Recreate videos).
bool CharacterSheetUseful(const std::wstring& image); // "Clear all..." with its confirmation: every saved character leaves the list (their pictures stay)
// While Star in a video's "Choose from the gallery" is on: a floating bar over the gallery (Use this picture / Cancel)
void StarPickBar(AppState& app);
// The same bar for another page: `question` over it, `use` gets the picture clicked (then that page's tab is the caller's to open)
void PickPictureFromGallery(AppState& app, const std::string& question, std::function<void(AppState&, const std::wstring&)> use);
void SetSwapScene(AppState& app, const std::wstring& path);        // opens the Characters tab with this picture as the scene to swap into
std::wstring PasteBoardImage(std::string& message);
// "Same scene, other characters" (Vision board): a window where each cast of characters gets its own prompt.
void OpenRecast();
void DrawRecastWindow(AppState& app);
void PumpRecastResults(AppState& app);
BoardPromptOptions BoardCurrentOptions(const BoardViewState& st); // the Vision board's prompt settings as they are now                // a picture from the clipboard as a file ("" = none, message says why)
std::string ComfyTabLabel(AppState& app);        // "ComfyUI" or "ComfyUI (2)" while jobs run
// Puts pictures on the Vision board (kind: 0 character, 1 background, 2 item) and shows that tab.
void AddImagesToBoard(AppState& app, const std::vector<std::wstring>& paths, int kind);
void PumpWorkflows(AppState& app); // each frame: library list + batch extraction results
// Opens the image's ComfyUI workflow in the node viewer (switches to that tab).
void OpenInNodeViewer(AppState& app, const std::wstring& imagePath, const ImageMetadata& meta);
void DrawCleanExportWindow(AppState& app);
void DrawRenameWindow(AppState& app);
// Returns true when the theme or UI scale changed and the UI must be rebuilt.
bool DrawAppearanceWindow(AppState& app);
