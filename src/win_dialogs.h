#pragma once
#include <string>
#include <vector>
#include <windows.h>

// Native Explorer-style pickers (IFileOpenDialog/IFileSaveDialog). Return an
// empty string if the user cancels. CoInitializeEx must already have been
// called on the calling thread.
std::wstring OpenImageFileDialog(HWND owner);
std::wstring OpenFolderDialog(HWND owner);
std::wstring SaveCsvJsonFileDialog(HWND owner, bool json);
std::wstring OpenCsvJsonFileDialog(HWND owner);
// Where to save a PNG picture (suggestedName: the file name offered).
std::wstring SavePngFileDialog(HWND owner, const std::wstring& suggestedName);
// A ComfyUI workflow (.json) or an image that has one embedded.
std::wstring OpenWorkflowFileDialog(HWND owner);

// Moves the given files to the Recycle Bin (reversible), showing Explorer's
// own progress UI but not its confirmation prompt (the caller is expected to
// confirm first). Returns false only if the shell reports every file failed.
bool DeleteFilesToRecycleBin(HWND owner, const std::vector<std::wstring>& paths, std::wstring* errorOut);
