#pragma once
#include <string>
#include <vector>
#include <set>

struct TagCategory {
    std::string name;
    std::vector<std::string> tags;
    bool defaultEnabled;
};

// Curated, fully offline pools of generic (non-copyright) Danbooru-style
// descriptive tags, grouped by category, for prompt inspiration.
const std::vector<TagCategory>& GetTagCategories();

// Picks `totalCount` unique tags at random from the tags belonging to the
// categories named in `enabledCategories` (matched against
// GetTagCategories()[i].name), plus any tags in `extraTags` (e.g. imported
// from a CSV/JSON scrape export). Returns fewer than totalCount if the
// combined pool is smaller.
std::vector<std::string> GenerateRandomTags(const std::set<std::string>& enabledCategories,
                                            const std::vector<std::string>& extraTags, int totalCount);
