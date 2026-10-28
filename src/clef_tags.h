#pragma once
// Clef reads each Vision library entry (its picture and its description) and tags it: genre, mood, setting, era and subject. The library can then be filtered and
// sorted by those. The tags are kept between sessions (clef_tags.json).
#include <string>
#include <vector>

struct ClefTags {
    int genre = -1, mood = -1, setting = -1, era = -1, subject = -1; // indices into ClefTagNames(0..4); -1 = not tagged
};

struct ClefTagInput {
    std::wstring imagePath;
    std::string text;       // the description (prompt, tags)
    long long mtime = 0;
};

const std::vector<std::string>& ClefTagNames(int kind); // 0 genre, 1 mood, 2 setting, 3 era, 4 subject
const char* ClefTagKindName(int kind);                  // "Genre"...
bool ClefTagsFor(const std::wstring& imagePath, ClefTags& out);
std::string ClefTagLine(const std::wstring& imagePath);  // "fantasy · warm mood · forest · high fantasy · a full-body character" ("" = not tagged)
unsigned ClefTagsVersion();                              // changes when tags are added

void ClefTagStart(const std::vector<ClefTagInput>& items, const std::string& comfyUrl, bool onlyNew);
void ClefTagStop();
bool ClefTagRunning();
void ClefTagProgress(int& done, int& total, std::string& phase);
