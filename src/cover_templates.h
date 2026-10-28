#pragma once
// Song covers from templates: your picture (squared) with the title designed on it by PhotoCraft (tools\photocraft, the same engine as Design):
// fades, type, shapes, layer styles and colour adjustments, no AI. Quick (about a second each), so every template can be previewed.
#include <string>

int CoverTemplateCount();
const char* CoverTemplateName(int index);
bool CoverTemplatesAvailable(); // photocraft-cli is there
// Renders one (blocks: a worker thread). `picture` may be empty: a background in the template's colours. `size`: the square's side in pixels.
// Returns "" when it worked, else why not.
std::string RenderCoverTemplate(int index, const std::wstring& picture, const std::string& title, const std::string& line2, const std::wstring& outPng, int size = 1080);
// The picture itself, squared (centre crop, a little above the middle where faces are): "Picture as is". "" = it worked.
std::string SquarePicture(const std::wstring& picture, const std::wstring& outPng, int size = 1080);
