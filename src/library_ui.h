#pragma once
// Poses & outfits (library_ui.cpp): your own copies of ギア's AI pose gallery and AI costume gallery (anhao/ai-pose-gallery-zh and
// Stone-dielianhua/ComfyUI_Costume-Gallery_ZH) in a folder of yours - their pictures may not be shared or sold, so the app never ships them.
// A picker shows them; an edit puts someone into a pose or an outfit (FLUX 3 Klein, the writer putting a drawn pose into words).
#include <functional>
#include <string>

class AppState;

struct LibItem {
    int kind = 0;               // 0 a pose, 1 an outfit
    std::string id;             // "full_single_P006_front", "SPR-0001"
    std::string group, category, angle; // in English (a pose: full body / upper body..., standing...; an outfit: summer...)
    std::string words;          // an outfit's English keywords ("ivory frill knit denim mini skirt coat")
    std::wstring thumb, image;  // its small and full pictures
};

bool LibraryHas(int kind);                       // the library folder has that library
void OpenLibraryPicker(AppState& app, int kind, const std::string& title, std::function<void(AppState&, const LibItem&)> use);
void DrawLibraryPicker(AppState& app);           // once a frame: the picker's window when open
// `person` put into the pose / dressed in the outfit (a new picture, full body on a plain backdrop); `done` gets it ("" and why, if it failed)
void LibraryEdit(AppState& app, const std::wstring& person, const LibItem& item,
                 std::function<void(AppState&, const std::wstring& made, const std::string& why)> done);
void PumpLibrary(AppState& app);                 // once a frame: the edits coming back
int LibraryEditsBusy();                          // edits under way
inline int LibraryPosePeople(const std::string& poseId) { return poseId.rfind("pair", 0) == 0 ? 2 : 1; } // how many people a pose is for
std::string LibraryPoseWords(const std::string& poseId); // the writer's words for a pose, once it described it ("" = not yet)
// The writer puts a pose into words (kept for next time); `done` gets them on the UI thread ("" = no writer / no answer)
void LibraryDescribePose(AppState& app, const LibItem& pose, std::function<void(AppState&, const std::string& words)> done);
void DrawLibraryFolderSetting();                 // the folder, with Change... (Options)
