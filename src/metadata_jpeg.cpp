#include "metadata_jpeg.h"
#include "exif_util.h"
#include <fstream>
#include <cstring>

namespace {

uint16_t ReadBE16(const unsigned char* p) { return (uint16_t(p[0]) << 8) | p[1]; }
uint32_t ReadLE32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

const char kExifPrefix[6] = {'E', 'x', 'i', 'f', 0, 0};
const char kXmpPrefix[] = "http://ns.adobe.com/xap/1.0/";

void EmitExif(const ExifResult& exif, std::vector<RawField>& out) {
    if (!exif.found) return;
    if (!exif.userComment.empty()) out.push_back({"EXIF:UserComment", exif.userComment});
    if (!exif.imageDescription.empty()) out.push_back({"EXIF:ImageDescription", exif.imageDescription});
    // ComfyUI's WebP/JPEG output: "workflow:{...}" / "prompt:{...}" in the Make / Model tags.
    for (const std::string* v : {&exif.make, &exif.model}) {
        for (const char* key : {"workflow", "prompt"}) {
            const size_t n = std::strlen(key);
            if (v->size() > n + 1 && v->compare(0, n, key) == 0 && (*v)[n] == ':') {
                out.push_back({key, v->substr(n + 1)});
                break;
            }
        }
    }
}

} // namespace

bool ReadJpegTextFields(const std::wstring& path, std::vector<RawField>& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "Could not open file";
        return false;
    }
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (buf.size() < 4 || buf[0] != 0xFF || buf[1] != 0xD8) {
        if (error) *error = "Not a JPEG file";
        return false;
    }

    size_t pos = 2;
    while (pos + 4 <= buf.size()) {
        if (buf[pos] != 0xFF) break;
        unsigned char marker = buf[pos + 1];
        pos += 2;
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (marker == 0xD9 || marker == 0xDA) break; // EOI or start-of-scan: no more metadata segments

        if (pos + 2 > buf.size()) break;
        uint16_t segLen = ReadBE16(&buf[pos]);
        if (segLen < 2 || pos + segLen > buf.size()) break;
        const unsigned char* segData = &buf[pos + 2];
        size_t segDataLen = segLen - 2;

        if (marker == 0xE1) { // APP1: Exif or XMP
            if (segDataLen > 6 && memcmp(segData, kExifPrefix, 6) == 0) {
                ExifResult exif = ParseExifTiff(segData + 6, segDataLen - 6);
                EmitExif(exif, out);
            } else if (segDataLen > strlen(kXmpPrefix) && memcmp(segData, kXmpPrefix, strlen(kXmpPrefix)) == 0) {
                std::string xmp(reinterpret_cast<const char*>(segData), segDataLen);
                out.push_back({"XMP", xmp});
            }
        }

        pos += segLen;
    }

    return true;
}

bool ReadWebpTextFields(const std::wstring& path, std::vector<RawField>& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "Could not open file";
        return false;
    }
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (buf.size() < 12 || memcmp(&buf[0], "RIFF", 4) != 0 || memcmp(&buf[8], "WEBP", 4) != 0) {
        if (error) *error = "Not a WEBP file";
        return false;
    }

    size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        char fourcc[5] = {0};
        memcpy(fourcc, &buf[pos], 4);
        uint32_t chunkSize = ReadLE32(&buf[pos + 4]);
        size_t dataStart = pos + 8;
        if (dataStart + chunkSize > buf.size()) break;
        const unsigned char* data = &buf[dataStart];

        if (strcmp(fourcc, "EXIF") == 0) {
            if (chunkSize > 6 && memcmp(data, kExifPrefix, 6) == 0) {
                ExifResult exif = ParseExifTiff(data + 6, chunkSize - 6);
                EmitExif(exif, out);
            } else {
                ExifResult exif = ParseExifTiff(data, chunkSize);
                EmitExif(exif, out);
            }
        } else if (strcmp(fourcc, "XMP ") == 0) {
            std::string xmp(reinterpret_cast<const char*>(data), chunkSize);
            out.push_back({"XMP", xmp});
        }

        pos = dataStart + chunkSize + (chunkSize & 1); // chunks are padded to even size
    }

    return true;
}
