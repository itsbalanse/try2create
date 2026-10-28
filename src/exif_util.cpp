#include "exif_util.h"
#include <cstring>

namespace {

uint16_t Read16(const unsigned char* p, bool le) {
    return le ? (uint16_t(p[0]) | (uint16_t(p[1]) << 8)) : (uint16_t(p[1]) | (uint16_t(p[0]) << 8));
}

uint32_t Read32(const unsigned char* p, bool le) {
    return le ? (uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24))
              : (uint32_t(p[3]) | (uint32_t(p[2]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[0]) << 24));
}

size_t TypeSize(uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1; // BYTE, ASCII, SBYTE, UNDEFINED
        case 3: case 8: return 2;                 // SHORT, SSHORT
        case 4: case 9: case 11: return 4;        // LONG, SLONG, FLOAT
        case 5: case 10: case 12: return 8;        // RATIONAL, SRATIONAL, DOUBLE
        default: return 0;
    }
}

std::string Utf16ToUtf8(const unsigned char* data, size_t byteLen, bool le) {
    std::string out;
    for (size_t i = 0; i + 1 < byteLen; i += 2) {
        uint16_t u = Read16(data + i, le);
        if (u == 0) break;
        if (u < 0x80) {
            out += static_cast<char>(u);
        } else if (u < 0x800) {
            out += static_cast<char>(0xC0 | (u >> 6));
            out += static_cast<char>(0x80 | (u & 0x3F));
        } else {
            // Treat surrogate pairs as opaque BMP codepoints; good enough for prompt text.
            out += static_cast<char>(0xE0 | (u >> 12));
            out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (u & 0x3F));
        }
    }
    return out;
}

std::string DecodeUserComment(const unsigned char* data, size_t len) {
    if (len == 0) return {};
    if (len > 8) {
        if (memcmp(data, "ASCII\0\0\0", 8) == 0) {
            return std::string(reinterpret_cast<const char*>(data + 8), len - 8);
        }
        if (memcmp(data, "UNICODE\0", 8) == 0) {
            // Encoded with the same endianness as the surrounding TIFF; caller already
            // handles ASCII-only prompts in the overwhelming majority of real files, so
            // assume little-endian (the common case for JPEG/WEBP writers on Windows).
            return Utf16ToUtf8(data + 8, len - 8, true);
        }
        if (memcmp(data, "JIS\0\0\0\0\0", 8) == 0) {
            return {}; // not supported; rare in AI-generation tooling
        }
    }
    return std::string(reinterpret_cast<const char*>(data), len);
}

// Reads one IFD, returns offset of next IFD (0 if none), and invokes visitor for each entry.
template <typename Visitor>
uint32_t WalkIfd(const unsigned char* tiff, size_t len, uint32_t ifdOffset, bool le, Visitor&& visitor) {
    if (ifdOffset + 2 > len) return 0;
    uint16_t count = Read16(tiff + ifdOffset, le);
    size_t entriesStart = ifdOffset + 2;
    for (uint16_t i = 0; i < count; ++i) {
        size_t entryOff = entriesStart + size_t(i) * 12;
        if (entryOff + 12 > len) break;
        uint16_t tag = Read16(tiff + entryOff, le);
        uint16_t type = Read16(tiff + entryOff + 2, le);
        uint32_t cnt = Read32(tiff + entryOff + 4, le);
        size_t elemSize = TypeSize(type);
        size_t total = elemSize * (size_t)cnt;
        const unsigned char* valuePtr = tiff + entryOff + 8;
        const unsigned char* dataPtr = nullptr;
        if (total <= 4) {
            dataPtr = valuePtr;
        } else {
            uint32_t offset = Read32(valuePtr, le);
            if (offset + total > len) continue;
            dataPtr = tiff + offset;
        }
        visitor(tag, type, total, dataPtr);
    }
    size_t nextOff = entriesStart + size_t(count) * 12;
    if (nextOff + 4 > len) return 0;
    return Read32(tiff + nextOff, le);
}

} // namespace

ExifResult ParseExifTiff(const unsigned char* tiff, size_t len) {
    ExifResult result;
    if (len < 8) return result;
    bool le;
    if (memcmp(tiff, "II", 2) == 0) le = true;
    else if (memcmp(tiff, "MM", 2) == 0) le = false;
    else return result;

    if (Read16(tiff + 2, le) != 42) return result;
    uint32_t ifd0Offset = Read32(tiff + 4, le);

    uint32_t exifIfdOffset = 0;
    WalkIfd(tiff, len, ifd0Offset, le, [&](uint16_t tag, uint16_t /*type*/, size_t total, const unsigned char* data) {
        if (tag == 0x010E) { // ImageDescription
            result.imageDescription.assign(reinterpret_cast<const char*>(data), total);
            result.found = true;
        } else if (tag == 0x010F || tag == 0x0110) { // Make / Model
            std::string& dst = tag == 0x010F ? result.make : result.model;
            dst.assign(reinterpret_cast<const char*>(data), total);
            while (!dst.empty() && dst.back() == '\0') dst.pop_back();
            if (!dst.empty()) result.found = true;
        } else if (tag == 0x8769) { // Exif IFD pointer
            exifIfdOffset = Read32(data, le);
        }
    });

    if (exifIfdOffset != 0) {
        WalkIfd(tiff, len, exifIfdOffset, le, [&](uint16_t tag, uint16_t /*type*/, size_t total, const unsigned char* data) {
            if (tag == 0x9286) { // UserComment
                result.userComment = DecodeUserComment(data, total);
                if (!result.userComment.empty()) result.found = true;
            }
        });
    }

    return result;
}
