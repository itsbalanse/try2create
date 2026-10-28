#pragma once
// Import relations (what one character does to another) from a text file, one per line ("laughs with", "hangs out with"), and let Clef sort them into the
// categories of the relation library by himself. Without Clef they go into "My own".
#include <string>

class AppState;

// Starts reading the file and sorting it in the background.
void RelationImportStart(const std::wstring& txtPath, const std::string& comfyUrl);
bool RelationImportRunning();
std::string RelationImportStatus(); // what it is doing / what it did last ("" = nothing yet)
// Each frame: puts finished imports into the library (on the UI thread, where the library lives) and reports in the status line.
void PumpRelationImport(AppState& app);
