#include "metadata_png.h"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <cstring>
#include "stb_image.h" // for stbi_zlib_decode_malloc_guesssize_headerflag (zTXt/iTXt inflate)

namespace {

uint32_t ReadBE32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Inflates a zlib-wrapped buffer (used by zTXt and compressed iTXt).
bool ZlibInflate(const unsigned char* data, size_t len, std::string& out) {
    if (len == 0) return false;
    int outLen = 0;
    // Guess an initial output size proportional to input; stb grows it as needed.
    char* decoded = stbi_zlib_decode_malloc_guesssize_headerflag(
        reinterpret_cast<const char*>(data), static_cast<int>(len),
        static_cast<int>(len) * 4 + 256, &outLen, 1);
    if (!decoded) return false;
    out.assign(decoded, decoded + outLen);
    free(decoded);
    return true;
}

} // namespace

bool ReadPngTextChunks(const std::wstring& path, std::vector<RawField>& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "Could not open file";
        return false;
    }

    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    static const unsigned char kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (buf.size() < 8 || memcmp(buf.data(), kSig, 8) != 0) {
        if (error) *error = "Not a PNG file";
        return false;
    }

    size_t pos = 8;
    while (pos + 12 <= buf.size()) {
        uint32_t length = ReadBE32(&buf[pos]);
        if (pos + 8 + (size_t)length + 4 > buf.size()) break; // truncated/corrupt
        char type[5] = {0};
        memcpy(type, &buf[pos + 4], 4);
        const unsigned char* data = &buf[pos + 8];

        if (strcmp(type, "tEXt") == 0) {
            // keyword \0 text  (Latin-1 per spec, but tools commonly write UTF-8 directly)
            const unsigned char* nul = static_cast<const unsigned char*>(memchr(data, 0, length));
            if (nul) {
                size_t keyLen = nul - data;
                std::string keyword(reinterpret_cast<const char*>(data), keyLen);
                size_t textLen = length - keyLen - 1;
                std::string text(reinterpret_cast<const char*>(nul + 1), textLen);
                out.push_back({keyword, text});
            }
        } else if (strcmp(type, "zTXt") == 0) {
            const unsigned char* nul = static_cast<const unsigned char*>(memchr(data, 0, length));
            if (nul && (size_t)(nul - data) + 1 < length) {
                size_t keyLen = nul - data;
                std::string keyword(reinterpret_cast<const char*>(data), keyLen);
                // byte after NUL is compression method (must be 0 = deflate)
                const unsigned char* compData = nul + 2;
                size_t compLen = length - keyLen - 2;
                std::string text;
                if (ZlibInflate(compData, compLen, text)) {
                    out.push_back({keyword, text});
                }
            }
        } else if (strcmp(type, "iTXt") == 0) {
            // keyword \0 compression_flag(1) compression_method(1) language_tag \0 translated_keyword \0 text
            const unsigned char* p = data;
            const unsigned char* end = data + length;
            const unsigned char* nul1 = static_cast<const unsigned char*>(memchr(p, 0, end - p));
            if (nul1 && nul1 + 2 <= end) {
                std::string keyword(reinterpret_cast<const char*>(p), nul1 - p);
                unsigned char compressionFlag = *(nul1 + 1);
                p = nul1 + 3; // skip NUL, compression_flag, compression_method
                if (p <= end) {
                    const unsigned char* nul2 = static_cast<const unsigned char*>(memchr(p, 0, end - p));
                    if (nul2) {
                        p = nul2 + 1; // skip language tag
                        const unsigned char* nul3 = static_cast<const unsigned char*>(memchr(p, 0, end - p));
                        if (nul3) {
                            p = nul3 + 1; // skip translated keyword, text follows
                            size_t textLen = end - p;
                            std::string text;
                            if (compressionFlag != 0) {
                                ZlibInflate(p, textLen, text);
                            } else {
                                text.assign(reinterpret_cast<const char*>(p), textLen);
                            }
                            if (!text.empty()) out.push_back({keyword, text});
                        }
                    }
                }
            }
        } else if (strcmp(type, "IEND") == 0) {
            break;
        }

        pos += 8 + length + 4; // length + type + data + crc
    }

    return true;
}
