#pragma once
// Clef looks at the pictures and grades them: how explicit they are (the NSFW score), how good they look (the aesthetic quality, 0-10) and what kind of picture they are
// (photo, anime...). Its grades replace the ones of the small classifier models, and are kept between sessions (clef_grades.json).
#include <string>
#include <vector>

class AppState;

struct ClefGradeInput {
    std::wstring path;
    long long mtime = 0;
    unsigned long long size = 0;
};

// Starts a background job: Clef is loaded (taking the GPU from ComfyUI when needed), grades the pictures, and is closed again. onlyNew skips pictures already graded.
void ClefGradeStart(const std::vector<ClefGradeInput>& items, const std::string& comfyUrl, bool onlyNew);
void ClefGradeStop();
bool ClefGradeRunning();
void ClefGradeProgress(int& done, int& total, std::string& phase);
// Each frame: puts the grades on the pictures of the open folder (and shows the job's progress in the status line).
void PumpClefGrades(AppState& app);

// Prompt triage: Clef reads a prompt BEFORE any generation time is spent on it and says how clear and specific it is, whether it contradicts itself, whether it asks for too
// much for one picture, and how explicit it is. The answer is a few lines of text with advice. Runs in the background.
void ClefRatePromptStart(const std::string& prompt, const std::string& comfyUrl);
bool ClefRatePromptRunning();
// True once there is a finished answer for the last prompt asked (and it is handed over once): `forPrompt` is the prompt it was for.
bool ClefRatePromptTake(std::string& forPrompt, std::string& text, bool& good);
