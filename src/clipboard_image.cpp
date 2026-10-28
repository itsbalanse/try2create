#include "clipboard_image.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <shlobj.h>
#include <system_error>

#include "metadata_strip.h"
#include "nsfw_classifier.h" // DecodeImageFileRGBA
#include "stb_image_write.h"
#include "video_util.h"

namespace fs = std::filesystem;

namespace {

std::wstring TempDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    std::wstring dir = (n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring(L".\\")) + L"Try2Create\\clipboard";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

// forget copies made on earlier days
void CleanOldCopies(const std::wstring& dir) {
    std::error_code ec;
    const auto limit = fs::file_time_type::clock::now() - std::chrono::hours(24);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        if (it->is_regular_file(fe) && it->last_write_time(fe) < limit) fs::remove(it->path(), fe);
    }
}

std::string EncodePng(const std::vector<unsigned char>& rgba, int w, int h) {
    std::string out;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(size)); },
        &out, w, h, 4, rgba.data(), w * 4);
    return out;
}

bool ReadWholeFile(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

bool SetData(UINT format, const void* data, size_t size) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) return false;
    void* p = GlobalLock(mem);
    if (!p) {
        GlobalFree(mem);
        return false;
    }
    std::memcpy(p, data, size);
    GlobalUnlock(mem);
    if (!SetClipboardData(format, mem)) {
        GlobalFree(mem);
        return false;
    }
    return true;
}

} // namespace

bool CopyImagesToClipboard(HWND owner, const std::vector<std::wstring>& paths, bool stripMetadata, std::string* message) {
    auto fail = [&](const std::string& m) {
        if (message) *message = m;
        return false;
    };
    if (paths.empty()) return fail("Nothing to copy.");

    // ---- the files that go on the clipboard (clean copies in a temp folder when asked) ----
    const std::wstring temp = TempDir();
    CleanOldCopies(temp);
    std::vector<std::wstring> files;
    int unclean = 0;
    for (const std::wstring& src : paths) {
        if (!stripMetadata) {
            files.push_back(src);
            continue;
        }
        const fs::path p(src);
        if (IsVideoPath(src)) { // the streams copied as they are, without any tags (ComfyUI keeps the prompt and the workflow in them)
            const std::wstring ff = FfmpegPath();
            const std::wstring dst = temp + L"\\" + p.stem().wstring() + L"_clean" + p.extension().wstring();
            std::string log;
            std::error_code ec;
            if (!ff.empty() &&
                RunCaptureHidden(L"\"" + ff + L"\" -y -v error -i \"" + src + L"\" -map 0 -map_metadata -1 -map_chapters -1 -c copy -fflags +bitexact \"" + dst + L"\"", log, true) &&
                fs::exists(dst, ec) && fs::file_size(dst, ec) > 0) {
                files.push_back(dst);
            } else {
                files.push_back(src);
                ++unclean;
            }
            continue;
        }
        const bool container = CanStripMetadata(src);
        std::wstring dst = temp + L"\\" + p.stem().wstring() + L"_clean" + (container ? p.extension().wstring() : std::wstring(L".png"));
        std::string err;
        if (container) {
            if (!WriteCleanCopy(src, dst, StripMode::RemoveAll, "", &err)) return fail("Could not make a clean copy: " + err);
        } else {
            std::vector<unsigned char> px;
            int w = 0, h = 0;
            if (!DecodeImageFileRGBA(src, px, w, h)) return fail("Could not read that picture.");
            const std::string png = EncodePng(px, w, h);
            std::ofstream out(dst, std::ios::binary | std::ios::trunc);
            if (!out) return fail("Could not write a clean copy.");
            out.write(png.data(), static_cast<std::streamsize>(png.size()));
        }
        files.push_back(dst);
    }

    // ---- pixels of the first picture ----
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    const bool haveBitmap = DecodeImageFileRGBA(paths.front(), rgba, w, h) && w > 0 && h > 0 &&
                            static_cast<unsigned long long>(w) * static_cast<unsigned long long>(h) <= 150ull * 1000 * 1000;
    std::string pngBytes;
    if (haveBitmap) {
        // with metadata: the original PNG bytes; otherwise a fresh encode (carries nothing but pixels)
        const bool isPng = _wcsicmp(fs::path(files.front()).extension().c_str(), L".png") == 0;
        if (!(isPng && ReadWholeFile(files.front(), pngBytes))) pngBytes = EncodePng(rgba, w, h);
    }

    if (!OpenClipboard(owner)) return fail("The clipboard is busy; try again.");
    EmptyClipboard();
    bool any = false;

    if (haveBitmap) {
        // CF_DIBV5: 32-bit BGRA with alpha, bottom-up (Windows derives the plain bitmap formats from it)
        BITMAPV5HEADER hdr{};
        hdr.bV5Size = sizeof(hdr);
        hdr.bV5Width = w;
        hdr.bV5Height = h;
        hdr.bV5Planes = 1;
        hdr.bV5BitCount = 32;
        hdr.bV5Compression = BI_BITFIELDS;
        hdr.bV5SizeImage = static_cast<DWORD>(static_cast<size_t>(w) * h * 4);
        hdr.bV5RedMask = 0x00FF0000;
        hdr.bV5GreenMask = 0x0000FF00;
        hdr.bV5BlueMask = 0x000000FF;
        hdr.bV5AlphaMask = 0xFF000000;
        hdr.bV5CSType = LCS_sRGB;
        hdr.bV5Intent = LCS_GM_IMAGES;
        std::string dib(sizeof(hdr) + hdr.bV5SizeImage, '\0');
        std::memcpy(dib.data(), &hdr, sizeof(hdr));
        unsigned char* dst = reinterpret_cast<unsigned char*>(dib.data()) + sizeof(hdr);
        for (int y = 0; y < h; ++y) {
            const unsigned char* s = &rgba[static_cast<size_t>(h - 1 - y) * w * 4];
            unsigned char* d = dst + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                d[x * 4 + 0] = s[x * 4 + 2];
                d[x * 4 + 1] = s[x * 4 + 1];
                d[x * 4 + 2] = s[x * 4 + 0];
                d[x * 4 + 3] = s[x * 4 + 3];
            }
        }
        any |= SetData(CF_DIBV5, dib.data(), dib.size());
        if (!pngBytes.empty()) any |= SetData(RegisterClipboardFormatW(L"PNG"), pngBytes.data(), pngBytes.size());
    }

    // the files themselves (what Explorer and upload boxes paste)
    {
        size_t chars = 1;
        for (const std::wstring& f : files) chars += f.size() + 1;
        std::string drop(sizeof(DROPFILES) + chars * sizeof(wchar_t), '\0');
        DROPFILES df{};
        df.pFiles = sizeof(DROPFILES);
        df.fWide = TRUE;
        std::memcpy(drop.data(), &df, sizeof(df));
        wchar_t* w2 = reinterpret_cast<wchar_t*>(drop.data() + sizeof(DROPFILES));
        for (const std::wstring& f : files) {
            std::memcpy(w2, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
            w2 += f.size() + 1;
        }
        any |= SetData(CF_HDROP, drop.data(), drop.size());
        const DWORD effect = DROPEFFECT_COPY;
        SetData(RegisterClipboardFormatW(L"Preferred DropEffect"), &effect, sizeof(effect));
    }
    CloseClipboard();
    if (!any) return fail("Could not put the picture on the clipboard.");
    if (message) {
        *message = std::string(stripMetadata ? "Copied without metadata" : "Copied") +
                   (files.size() > 1 ? " (" + std::to_string(files.size()) + " pictures)" : std::string()) +
                   ". Paste it into a chat, Paint or Explorer." + (unclean ? " (A clip could not be cleaned: it was copied unchanged.)" : "");
    }
    return true;
}
