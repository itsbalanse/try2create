#include "metadata_strip.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <vector>

namespace fs = std::filesystem;

namespace {

uint32_t ReadBE32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void AppendBE32(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v >> 24));
    s.push_back(static_cast<char>(v >> 16));
    s.push_back(static_cast<char>(v >> 8));
    s.push_back(static_cast<char>(v));
}

uint32_t ReadLE32(const unsigned char* p) {
    return p[0] | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void AppendLE32(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v));
    s.push_back(static_cast<char>(v >> 8));
    s.push_back(static_cast<char>(v >> 16));
    s.push_back(static_cast<char>(v >> 24));
}

uint32_t Crc32(const unsigned char* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::string MakePngTextChunk(const std::string& keyword, const std::string& text) {
    std::string data = keyword;
    data.push_back('\0');
    data += text;
    std::string chunk;
    AppendBE32(chunk, static_cast<uint32_t>(data.size()));
    chunk += "tEXt";
    chunk += data;
    uint32_t crc = Crc32(reinterpret_cast<const unsigned char*>(chunk.data() + 4), data.size() + 4);
    AppendBE32(chunk, crc);
    return chunk;
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool WriteFileBytes(const std::wstring& path, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

} // namespace

// ---------------------------------------------------------------------------
// PNG: keep the structural chunks and the ones needed to display the image
// correctly (colour, transparency, animation); drop every other ancillary
// chunk, which removes tEXt/zTXt/iTXt (prompts), eXIf, tIME and any
// tool-specific extras.
// ---------------------------------------------------------------------------
bool StripPng(const std::string& in, std::string& out, StripMode mode, const std::string& promptText) {
    static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (in.size() < 8 + 12 || memcmp(in.data(), kSig, 8) != 0) return false;
    static const std::set<std::string> kKeep = {"tRNS", "gAMA", "cHRM", "sRGB", "iCCP", "sBIT", "bKGD", "hIST",
                                                "pHYs", "sPLT", "acTL", "fcTL", "fdAT", "cICP", "mDCV", "cLLI"};
    const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data());
    size_t pos = 8;
    out.assign(in.data(), 8);
    bool sawIhdr = false, sawIend = false, textWritten = false;
    while (pos + 12 <= in.size()) {
        uint32_t len = ReadBE32(p + pos);
        if (len > 0x7FFFFFFFu || pos + 12 + static_cast<size_t>(len) > in.size()) return false;
        std::string type(in.data() + pos + 4, 4);
        const size_t total = 12 + static_cast<size_t>(len);
        const bool critical = (type[0] >= 'A' && type[0] <= 'Z');
        if (type == "IHDR") sawIhdr = true;
        if (critical || kKeep.count(type)) {
            // The prompt goes right before the image data.
            if (mode == StripMode::KeepPromptOnly && !textWritten && type == "IDAT" && !promptText.empty()) {
                out += MakePngTextChunk("parameters", promptText);
                textWritten = true;
            }
            out.append(in.data() + pos, total);
        }
        pos += total;
        if (type == "IEND") {
            sawIend = true;
            break;
        }
    }
    return sawIhdr && sawIend;
}

// ---------------------------------------------------------------------------
// JPEG: walk the marker segments up to the scan; drop EXIF/XMP (APP1), APP3-13
// (IPTC, Photoshop, ...), APP15 and comments. APP0 (JFIF), APP2 (ICC profile,
// but not the MPF index), APP14 (Adobe colour transform) and all coding tables
// stay. Everything from the first SOS on is copied untouched.
// ---------------------------------------------------------------------------
bool StripJpeg(const std::string& in, std::string& out) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data());
    if (in.size() < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;
    out.assign(in.data(), 2);
    size_t pos = 2;
    while (pos + 4 <= in.size()) {
        if (p[pos] != 0xFF) return false;
        unsigned char marker = p[pos + 1];
        if (marker == 0xFF) { ++pos; continue; } // fill byte
        if (marker == 0xDA) { // start of scan: the rest is entropy-coded data up to EOI
            out.append(in.data() + pos, in.size() - pos);
            return true;
        }
        if (marker == 0xD9) { out.append(in.data() + pos, in.size() - pos); return true; }
        if ((marker >= 0xD0 && marker <= 0xD8) || marker == 0x01) { // markers without a length
            out.append(in.data() + pos, 2);
            pos += 2;
            continue;
        }
        size_t len = (static_cast<size_t>(p[pos + 2]) << 8) | p[pos + 3];
        if (len < 2 || pos + 2 + len > in.size()) return false;
        bool drop = false;
        if (marker == 0xE1 || (marker >= 0xE3 && marker <= 0xED) || marker == 0xEF || marker == 0xFE) drop = true;
        if (marker == 0xE2 && len >= 6 && memcmp(p + pos + 4, "MPF\0", 4) == 0) drop = true;
        if (!drop) out.append(in.data() + pos, 2 + len);
        pos += 2 + len;
    }
    return false;
}

// ---------------------------------------------------------------------------
// WebP (RIFF): drop the EXIF and XMP chunks, clear their flags in VP8X and
// rewrite the RIFF size.
// ---------------------------------------------------------------------------
bool StripWebp(const std::string& in, std::string& out) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data());
    if (in.size() < 12 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WEBP", 4) != 0) return false;
    std::string body = "WEBP";
    size_t pos = 12;
    const size_t riffEnd = std::min(in.size(), static_cast<size_t>(ReadLE32(p + 4)) + 8);
    while (pos + 8 <= riffEnd) {
        std::string type(in.data() + pos, 4);
        uint32_t len = ReadLE32(p + pos + 4);
        size_t padded = static_cast<size_t>(len) + (len & 1);
        if (pos + 8 + padded > in.size()) return false;
        if (type != "EXIF" && type != "XMP ") {
            std::string chunk(in.data() + pos, 8 + padded);
            if (type == "VP8X" && len >= 1) chunk[8] = static_cast<char>(static_cast<unsigned char>(chunk[8]) & ~0x0C);
            body += chunk;
        }
        pos += 8 + padded;
    }
    out = "RIFF";
    AppendLE32(out, static_cast<uint32_t>(body.size()));
    out += body;
    return true;
}

bool CanStripMetadata(const std::wstring& path) {
    std::wstring ext = fs::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp";
}

bool WriteCleanCopy(const std::wstring& src, const std::wstring& dst, StripMode mode, const std::string& promptText,
                    std::string* error) {
    std::string in, out;
    if (!ReadFileBytes(src, in) || in.empty()) {
        if (error) *error = "Could not read the file";
        return false;
    }
    bool ok = false;
    if (in.size() > 8 && static_cast<unsigned char>(in[0]) == 0x89 && in[1] == 'P') ok = StripPng(in, out, mode, promptText);
    else if (in.size() > 3 && static_cast<unsigned char>(in[0]) == 0xFF && static_cast<unsigned char>(in[1]) == 0xD8) ok = StripJpeg(in, out);
    else if (in.size() > 12 && memcmp(in.data(), "RIFF", 4) == 0 && memcmp(in.data() + 8, "WEBP", 4) == 0) ok = StripWebp(in, out);
    else {
        out = in; // BMP, GIF, ...: nothing embedded that we can safely remove
        ok = true;
    }
    if (!ok) {
        if (error) *error = "The file is damaged or not in the expected format";
        return false;
    }
    if (!WriteFileBytes(dst, out)) {
        if (error) *error = "Could not write the copy";
        return false;
    }
    return true;
}
