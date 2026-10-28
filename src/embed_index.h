#pragma once
// Pictures turned into vectors by the embedding model (EmbeddingGemma-2), kept in a cache (embeddings.bin) so each picture is read once. A search text is turned into a vector
// the same way, and the pictures whose vectors are closest to it are the ones that mean the same. "More like this" uses a picture's own vector as the query.
#include <string>
#include <vector>

class AppState;

struct EmbedItem {
    std::wstring path;
    long long mtime = 0;
    unsigned long long size = 0;
};

// Starts a background job that embeds the pictures that have no vector yet (the model is loaded for it, taking the GPU from ComfyUI when needed).
void EmbedIndexStart(const std::vector<EmbedItem>& items, const std::string& comfyUrl);
void EmbedIndexStop();
bool EmbedIndexRunning();
void EmbedIndexProgress(int& done, int& total, std::string& phase);
unsigned EmbedIndexVersion();                                  // changes when vectors are added
bool EmbedHas(const EmbedItem& item);                          // is there a vector for this picture (and file version)
bool EmbedGet(const EmbedItem& item, std::vector<float>& out);

// The vector of a search text (a background request). EmbedQueryTake hands over the finished one once.
void EmbedQueryStart(const std::string& text, const std::string& comfyUrl);
bool EmbedQueryRunning();
bool EmbedQueryTake(std::string& text, std::vector<float>& vec);

// The similarity (cosine, -1..1) of `query` to each picture of the folder; -2 where the picture has no vector yet.
std::vector<float> EmbedScoreEntries(const AppState& app, const std::vector<float>& query);
float EmbedDot(const std::vector<float>& a, const std::vector<float>& b);

// Each frame: finished searches and "more like this" become the gallery's query; shows the indexing progress in the status line.
void PumpEmbed(AppState& app);
// "More like this": the gallery shows the pictures closest to this one.
void EmbedFindLike(AppState& app, int entryIndex);
