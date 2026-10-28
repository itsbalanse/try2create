#pragma once
// Puts pictures on the Windows clipboard so they can be pasted: as a bitmap + PNG (Paint, chat apps, browsers, editors)
// and as files (Explorer, upload boxes). With `stripMetadata` the copy carries no prompt, seed or EXIF.
#include <string>
#include <vector>
#include <windows.h>

// The first path is also copied as pixels; every path is copied as a file. Returns false with `message` on failure.
bool CopyImagesToClipboard(HWND owner, const std::vector<std::wstring>& paths, bool stripMetadata, std::string* message);
