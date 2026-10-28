#pragma once
// The Vision board: a canvas of picture cards (characters, a background, items, notes) joined by labelled relations
// ("Knight fights Dragon"), that turns into one scene prompt. A board can have several scenes and a timeline of beats
// ("0-10 s the fight, 15 s she pulls out a water gun, then we cut to the plane scene"), and every character carries an
// inventory (what they wear, hold and carry, with pictures) that belongs to the card on that board (a saved character brings its saved inventory along).
#include <string>
#include <utility>
#include <vector>

enum class BoardKind { Character, Background, Item, Note };

const char* BoardKindName(BoardKind kind);

struct BoardNode {
    int id = 0;
    BoardKind kind = BoardKind::Character;
    int scene = 0;        // which scene of the board it is in
    float x = 0, y = 0;   // top-left, in board units
    float w = 230;        // card width (the height follows from the picture and text)
    std::wstring image;   // picture file (empty for notes)
    std::string title;    // "Knight"
    std::string text;     // what it is, in words: appearance / place / object / the note itself
    float textH = 0;      // height of the text area in board units (0 = four lines); dragging the top / bottom edge of the card changes it
    std::string profile;  // characters: key of THIS card's own CharacterProfile (its inventory); the same picture in other beats of the board shares it
    bool describing = false; // waiting for the vision model (not saved)
};

struct BoardLink {
    int id = 0;
    int from = 0, to = 0; // node ids; reads "<from> <label> <to>"
    std::string label;    // "fights", "holds", "stands in"
};

// One beat of the video: its own board (cards and relations) shown from `start` to `end` seconds.
// ("0-3 s the guy fights another guy", "4-8 s he defends against a guy with a chair"...)
struct BoardScene {
    std::string name;     // optional ("Plane fight"); empty = "Beat N"
    float start = 0, end = 3;
    std::string text;     // what happens in this beat, in words (added to what the relations say)
    // camera and lighting (0 = auto: not mentioned in the prompt); indices into CameraChoices()
    int shot = 0, angle = 0, lens = 0, movement = 0, lighting = 0, dof = 0;
};

// Camera / lighting options. kind: 0 shot size, 1 angle, 2 lens, 3 movement (video), 4 lighting, 5 depth of field.
struct CameraChoice {
    const char* label;  // shown in the list
    const char* phrase; // what goes into the prompt ("" = auto)
};
const std::vector<CameraChoice>& CameraChoices(int kind);
// "medium shot, framed from the waist up, eye-level angle, soft golden-hour light" ("" when everything is auto)
std::string CameraText(const BoardScene& scene, bool includeMovement);

struct Board {
    std::vector<BoardNode> nodes;
    std::vector<BoardLink> links;
    std::vector<BoardScene> scenes{BoardScene()}; // the beats, in time order
    int nextId = 1;
    float panX = 0, panY = 0, zoom = 1;

    BoardNode* Find(int id);
    const BoardNode* Find(int id) const;
    int Add(BoardKind kind, const std::wstring& image, const std::string& text, float x, float y, int scene = 0);
    int Connect(int from, int to, const std::string& label = std::string()); // <0 when not allowed / already there
    void Remove(int nodeId);
    void RemoveLink(int linkId);
    std::string DisplayName(const BoardNode& n) const; // title, or "Character 2" when it has none
    std::string SceneName(int scene) const; // the name, or "Beat N"
    float TotalSec() const;                 // end of the last beat
    int AddScene(const std::string& name = std::string()); // a blank beat after the last one
    // A beat right after `after` (same length): with `copy` it starts as a copy of that beat's cards and relations.
    int AddSceneAfter(int after, bool copy);
    void RemoveScene(int scene); // its cards and relations go with it (the last beat stays)
    // Timeline editing (like a video editor):
    int SplitScene(int scene, float t);   // cut the beat in two at time t (both halves keep the board); -1 if t is not inside it
    int CloneScene(int scene);            // a copy right after it, later beats move over to make room
    void Ripple(int fromIndex);           // push beats after `fromIndex` later so none overlaps the one before
    void CloseGap(int scene);             // after removing / shortening: pull the later beats earlier to fill the hole
    bool SwapScene(int scene, int dir);   // move a beat earlier (-1) or later (+1) in the order, keeping lengths and gaps
    // Puts the beats in time order (cards follow their beat); `current` is renumbered to follow its beat.
    void SortScenes(int* current);
};

// Relation labels that make sense between two kinds of cards (the first is the default).
const std::vector<const char*>& BoardRelationPresets(BoardKind from, BoardKind to);
// The relations to pick from between two kinds of cards: the built-in ones, then the ones you saved, each with a category
// ("Fighting", "Friends & team", "Couple", "Funny"...), in category order.
struct RelationEntry {
    std::string label, category;
    bool mine = false; // one you saved yourself (can be removed)
};
std::vector<RelationEntry> BoardRelations(BoardKind from, BoardKind to);
std::vector<std::string> RelationCategories();
void AddMyRelation(BoardKind from, BoardKind to, const std::string& label, const std::string& category);
void RemoveMyRelation(const std::string& label);

// ---------------------------------------------------------------------------------------------------------------
// Characters and their inventory. A character is a picture: the same file is the same character on every board.
enum class InvSlot { Wearing, Holding, Carrying, Vehicle }; // Vehicle: parked / standing nearby, ridden or piloted, never carried
const char* InvSlotName(InvSlot slot);

struct InvItem {
    int id = 0;
    InvSlot slot = InvSlot::Holding;
    std::string name;     // "water gun"
    std::string text;     // optional detail: "bright blue, oversized"
    std::wstring image;   // picture kept in the items folder (may be empty)
};

struct CharacterProfile {
    std::string key;      // CharacterKey(image)
    std::wstring image;
    std::string name;
    std::string description; // the card text that goes with the character
    bool saved = false;      // kept in the "Saved characters" list
    std::wstring sheet;      // its character sheet (front, side, back views and expressions), "" = none
    int sheetNeeded = -1;    // does the sheet show what the picture cannot (a back that looks unlike the front)? 1 yes, 0 no, -1 not measured yet
    std::wstring orbit;      // its 360-degree view: an orbit video the Characters tab turns like a turntable, "" = none
    std::string backstory;   // who they are: age, how they act, walk and talk, likes, childhood, school, work... ("Age: 34\n..."), "" = not written yet
    std::vector<InvItem> items;
    int nextItemId = 1;
    InvItem& AddItem(InvSlot slot, const std::string& name, const std::wstring& image);
};

std::string CharacterKey(const std::wstring& image);
CharacterProfile* FindProfile(const std::wstring& image);
CharacterProfile& EnsureProfile(const std::wstring& image);
std::vector<CharacterProfile>& AllProfiles();
// A card has its own inventory (what it wears, holds and carries on THIS board): a fresh card starts empty, and a SAVED character
// (Characters tab / "Save character") brings its saved inventory along. The same picture in another beat of the board shares it.
CharacterProfile* ProfileOf(const BoardNode& node);                      // null when the card has none (yet)
CharacterProfile& EnsureProfileOf(Board& board, int nodeId, bool inheritUnsaved = false);
void DiscardProfile(const std::string& key); // forgets a temporary card inventory (never a saved character)
void SaveProfiles();
// "wearing a cape and boots; holding a water gun (bright blue)" - empty when there is nothing
std::string InventoryText(const CharacterProfile& profile);

// Item pictures kept for later use (copied into the items folder, never touched again).
std::wstring ItemsDir();
struct SavedItem {
    std::wstring path;
    std::string name;
};
std::vector<SavedItem> ListSavedItems();
bool RenameSavedItem(const std::wstring& path, const std::string& name); // the name shown for a saved picture ("My icons")
// Copies `source` into the items folder (a name taken from `name`) and remembers the name. Empty on failure.
std::wstring StoreItemImage(const std::wstring& source, const std::string& name);
// Built-in icons (clothes, bags, vehicles, tools...): small SVG drawings rasterized to PNG files on first use.
struct BuiltinIcon {
    std::string id, name, category;
    std::string desc; // for scenes: what it is, in words
};
const std::vector<BuiltinIcon>& BuiltinIcons();
std::wstring BuiltinIconImage(const std::string& id); // the PNG of an icon (made when first asked for); empty on failure
// Downloads a picture from a web address into the items folder (blocking: run it on a worker thread).
bool DownloadItemImage(const std::string& url, const std::string& name, std::wstring& storedPath, std::string& error);

// ---------------------------------------------------------------------------------------------------------------
struct BoardPromptOptions {
    int format = 0;           // 0 natural paragraph, 1 tag list, 2 video (uses the timeline)
    int scene = 0;            // which scene paragraph / tags describe
    int seconds = 10;         // unused when the board has beats
    std::string style;        // style directive ("" = none)
    std::string extra;        // extra direction from the user (camera, mood, ...)
    bool attachImages = false; // the model also gets the pictures (cards and inventory items)
};

// The quick draft: built from the cards and relations with no model.
std::string AssembleBoardPrompt(const Board& board, const BoardPromptOptions& options);
// The request for a model: the whole board described, and the pictures to attach (in the order they are numbered).
std::string BuildBoardBrief(const Board& board, const BoardPromptOptions& options, std::vector<std::wstring>& attachedImages);

// "Prepare for generation": copies every picture of the board (cards and inventory items) into a folder of its own, ready to
// drag or paste into ComfyUI or any other generator, and writes the prompt beside them.
std::wstring DefaultExportDir(); // Pictures\Vision Board Scenes
struct BoardExportResult {
    std::wstring folder;
    int pictures = 0;
    std::vector<std::wstring> files; // the copied pictures
    std::string error;
};
BoardExportResult ExportBoardForGeneration(const Board& board, const std::string& boardName, const std::wstring& rootDir,
                                           const std::string& finalPrompt, const BoardPromptOptions& options);

// Saving and loading (JSON).
std::wstring BoardsDir();
bool SaveBoard(const Board& board, const std::wstring& path);
bool LoadBoard(const std::wstring& path, Board& board);
std::vector<std::wstring> ListBoards(); // file paths, newest first

struct BoardCharRef {
    std::string key;
    std::wstring image;
    std::string name;
};
struct BoardInfo {
    std::wstring path;
    std::string name;       // file name without .json
    long long modified = 0; // unix seconds
    int cards = 0, relations = 0, scenes = 1;
    float totalSec = 0;
    std::wstring thumb;     // a picture that stands for the board
    std::vector<BoardCharRef> characters;
};
std::vector<BoardInfo> ScanBoards(); // every saved board, newest first (reads all the files: do not call every frame)
