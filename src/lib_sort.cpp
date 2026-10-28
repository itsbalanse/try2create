#include "lib_sort.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace {

const std::unordered_set<std::string>& Ignored() {
    // quality / boilerplate tags say nothing about what a picture shows
    static const std::unordered_set<std::string> s = {
        "masterpiece", "best quality", "high quality", "highres", "absurdres", "detailed", "highly detailed", "ultra detailed",
        "extremely detailed", "8k", "4k", "hdr", "uhd", "sharp focus", "professional", "award winning", "trending on artstation",
        "intricate", "intricate details", "newest", "very aesthetic", "aesthetic", "amazing quality", "score 9", "score 8 up", "score 7 up",
        "simple background", "looking at viewer", "photo", "image", "picture"};
    return s;
}

const std::unordered_set<std::string>& StopWords() {
    static const std::unordered_set<std::string> s = {
        "with", "that", "this", "from", "their", "there", "which", "while", "about", "above", "across", "along", "among", "around", "behind",
        "below", "beside", "between", "beyond", "could", "would", "should", "being", "have", "has", "had", "into", "onto", "over", "under",
        "through", "toward", "towards", "upon", "very", "more", "most", "some", "such", "than", "then", "them", "these", "those", "they",
        "what", "when", "where", "whose", "will", "your", "also", "each", "other", "only", "same", "just", "like", "image", "picture",
        "photo", "shows", "showing", "features", "featuring", "appears", "looks", "looking", "seen", "visible", "slightly", "small", "large",
        "background", "foreground", "style", "scene", "overall", "composition", "lighting", "colors", "color", "detail", "details", "detailed"};
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> TokensOf(const SortDoc& d) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](std::string t) {
        if (t.size() < 3 || t.size() > 32 || Ignored().count(t) || !seen.insert(t).second) return;
        out.push_back(std::move(t));
    };
    if (!d.tags.empty()) {
        std::string cur;
        int depth = 0;
        auto flush = [&]() {
            std::string t = Trim(cur);
            cur.clear();
            // weights like "(blue hair:1.2)" -> "blue hair"
            t.erase(std::remove(t.begin(), t.end(), '('), t.end());
            t.erase(std::remove(t.begin(), t.end(), ')'), t.end());
            t.erase(std::remove(t.begin(), t.end(), '['), t.end());
            t.erase(std::remove(t.begin(), t.end(), ']'), t.end());
            const size_t colon = t.rfind(':');
            if (colon != std::string::npos && colon + 1 < t.size() && std::isdigit(static_cast<unsigned char>(t[colon + 1]))) t = Trim(t.substr(0, colon));
            std::replace(t.begin(), t.end(), '_', ' ');
            std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            add(std::move(t));
        };
        for (char c : d.tags) {
            if (c == '(' || c == '[') ++depth;
            if (c == ')' || c == ']') depth = std::max(0, depth - 1);
            if ((c == ',' || c == '\n') && depth == 0) flush();
            else cur += c;
        }
        flush();
    }
    if (out.empty() && !d.prompt.empty()) {
        std::string w;
        int taken = 0;
        auto flush = [&]() {
            if (w.size() >= 4 && !StopWords().count(w) && taken < 24 && seen.count(w) == 0) {
                seen.insert(w);
                out.push_back(w);
                ++taken;
            }
            w.clear();
        };
        for (unsigned char c : d.prompt) {
            if (std::isalpha(c)) w += static_cast<char>(std::tolower(c));
            else flush();
        }
        flush();
    }
    return out;
}

std::string Title(const std::string& t) {
    std::string out = t;
    bool up = true;
    for (char& c : out) {
        if (up && std::isalpha(static_cast<unsigned char>(c))) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        up = c == ' ' || c == '-';
    }
    return out;
}

} // namespace

std::vector<SortGroup> ProposeGroups(const std::vector<SortDoc>& docs, int minGroup, int maxGroups) {
    const int n = static_cast<int>(docs.size());
    std::vector<std::vector<std::string>> tokens(n);
    std::unordered_map<std::string, int> df;
    for (int i = 0; i < n; ++i) {
        tokens[i] = TokensOf(docs[i]);
        for (const std::string& t : tokens[i]) ++df[t];
    }
    std::vector<bool> assigned(n, false);
    std::vector<SortGroup> groups;
    const double maxShare = n >= 12 ? 0.7 : 1.0; // a tag on nearly everything groups nothing
    while (static_cast<int>(groups.size()) < maxGroups) {
        std::unordered_map<std::string, int> count;
        for (int i = 0; i < n; ++i) {
            if (assigned[i]) continue;
            for (const std::string& t : tokens[i]) ++count[t];
        }
        std::string best;
        double bestScore = 0;
        for (const auto& [tag, c] : count) {
            if (c < minGroup) continue;
            const double share = static_cast<double>(df[tag]) / n;
            if (share > maxShare) continue;
            const double score = c * (1.0 - 0.5 * share);
            if (score > bestScore || (score == bestScore && tag < best)) {
                bestScore = score;
                best = tag;
            }
        }
        if (best.empty()) break;
        SortGroup g;
        g.name = Title(best);
        for (int i = 0; i < n; ++i) {
            if (assigned[i]) continue;
            if (std::find(tokens[i].begin(), tokens[i].end(), best) != tokens[i].end()) {
                g.docs.push_back(i);
                assigned[i] = true;
            }
        }
        groups.push_back(std::move(g));
    }
    return groups;
}
