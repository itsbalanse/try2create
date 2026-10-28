#pragma once
// Suggests folders for Vision library pictures by the tags (or, without tags, the words of the prompt) they share.
#include <string>
#include <vector>

struct SortDoc {
    std::string tags;   // "1girl, city, night" or empty
    std::string prompt; // used for keywords when there are no tags
};

struct SortGroup {
    std::string name;            // suggested folder name
    std::vector<int> docs;       // indices into the input
};

// Greedy: the tag shared by the most still-unsorted pictures (but not by almost everything) becomes a folder,
// its pictures are taken out, and it repeats. Groups smaller than `minGroup` are never made.
std::vector<SortGroup> ProposeGroups(const std::vector<SortDoc>& docs, int minGroup, int maxGroups);
