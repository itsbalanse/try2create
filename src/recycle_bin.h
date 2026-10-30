// The app's own Recycle bin: what an x deletes only leaves the lists at first, the file stays where it is, so a slip is undone in a click. When the app closes
// everything in it goes to the Windows Recycle Bin (where it can still be got back). The list is kept on disk, so a crash loses nothing: what was in the bin stays
// in it until the next close.
#pragma once
#include <string>
#include <vector>

#include "comfy.h"

struct AppState;

struct BinItem {
    std::string name;
    int kind = 0;                    // 0 picture, 1 video, 2 3D object, 3 sound
    std::vector<std::wstring> files; // everything that goes together (a video and the files beside it; a 3D object's folder)
    std::wstring thumb;              // the picture to show
    bool hasResult = false;          // it was on the Made list: it goes back there
    ComfyResult result;
    long long when = 0;
};

void BinAdd(AppState& app, BinItem item);
bool InBin(const std::wstring& path);
int BinCount();
void BinFlush();                     // the app is closing: all of it to the Windows Recycle Bin
void DrawBinTab(AppState& app, float height);
