#include "tag_import.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string TrimField(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

// RFC4180-ish CSV parser: handles quoted fields (with embedded commas,
// newlines, and doubled "" for a literal quote), matching what this app's
// own CsvEscape() in scrape.cpp produces.
std::vector<std::vector<std::string>> ParseCsv(const std::string& content) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool inQuotes = false;
    size_t i = 0, n = content.size();
    bool anyContent = false;

    while (i < n) {
        char c = content[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < n && content[i + 1] == '"') {
                    field += '"';
                    i += 2;
                } else {
                    inQuotes = false;
                    i += 1;
                }
            } else {
                field += c;
                i += 1;
            }
            continue;
        }
        if (c == '"') {
            inQuotes = true;
            anyContent = true;
            i += 1;
        } else if (c == ',') {
            row.push_back(field);
            field.clear();
            anyContent = true;
            i += 1;
        } else if (c == '\r') {
            i += 1; // normalize CRLF/CR to the LF handling below
        } else if (c == '\n') {
            row.push_back(field);
            rows.push_back(row);
            row.clear();
            field.clear();
            i += 1;
        } else {
            field += c;
            anyContent = true;
            i += 1;
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(field);
        rows.push_back(row);
    }
    (void)anyContent;
    return rows;
}

void SplitAndCollect(const std::string& joined, const char* sep, std::set<std::string>& seen,
                     std::vector<std::string>& out) {
    size_t sepLen = strlen(sep);
    size_t pos = 0;
    while (pos <= joined.size()) {
        size_t next = joined.find(sep, pos);
        std::string tag = TrimField(next == std::string::npos ? joined.substr(pos) : joined.substr(pos, next - pos));
        if (!tag.empty() && seen.insert(tag).second) out.push_back(tag);
        if (next == std::string::npos) break;
        pos = next + sepLen;
    }
}

TagImportResult ImportFromCsv(const std::wstring& path) {
    TagImportResult result;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        result.message = "Could not open file";
        return result;
    }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3); // strip UTF-8 BOM
    }

    auto rows = ParseCsv(content);
    if (rows.empty()) {
        result.message = "Empty file";
        return result;
    }

    const auto& header = rows[0];
    int tagsCol = -1;
    for (size_t i = 0; i < header.size(); ++i) {
        if (_stricmp(header[i].c_str(), "Tags") == 0) {
            tagsCol = static_cast<int>(i);
            break;
        }
    }
    if (tagsCol < 0) {
        result.message = "No 'Tags' column found -- expected a CSV exported by this app's Scrape/Export feature";
        return result;
    }

    std::set<std::string> seen;
    int rowsWithTags = 0;
    for (size_t r = 1; r < rows.size(); ++r) {
        if (static_cast<size_t>(tagsCol) >= rows[r].size()) continue;
        const std::string& field = rows[r][tagsCol];
        if (field.empty()) continue;
        size_t before = result.tags.size();
        SplitAndCollect(field, "; ", seen, result.tags);
        if (result.tags.size() > before) rowsWithTags++;
    }

    result.success = true;
    result.message = "Imported " + std::to_string(result.tags.size()) + " unique tag(s) from " +
                      std::to_string(rowsWithTags) + " of " + std::to_string(rows.size() - 1) + " row(s)";
    return result;
}

TagImportResult ImportFromJson(const std::wstring& path) {
    TagImportResult result;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        result.message = "Could not open file";
        return result;
    }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    try {
        json arr = json::parse(content);
        if (!arr.is_array()) {
            result.message = "Expected a JSON array -- exported by this app's Scrape/Export feature";
            return result;
        }
        std::set<std::string> seen;
        int entriesWithTags = 0;
        for (auto& item : arr) {
            if (!item.is_object() || !item.contains("tags") || !item["tags"].is_array()) continue;
            size_t before = result.tags.size();
            for (auto& t : item["tags"]) {
                if (!t.is_string()) continue;
                std::string tag = TrimField(t.get<std::string>());
                if (!tag.empty() && seen.insert(tag).second) result.tags.push_back(tag);
            }
            if (result.tags.size() > before) entriesWithTags++;
        }
        result.success = true;
        result.message = "Imported " + std::to_string(result.tags.size()) + " unique tag(s) from " +
                          std::to_string(entriesWithTags) + " of " + std::to_string(arr.size()) + " entries";
    } catch (const std::exception& e) {
        result.message = std::string("Invalid or unreadable JSON: ") + e.what();
    }
    return result;
}

} // namespace

TagImportResult ImportTagsFromFile(const std::wstring& path) {
    std::wstring ext = fs::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext == L".json") return ImportFromJson(path);
    return ImportFromCsv(path);
}
