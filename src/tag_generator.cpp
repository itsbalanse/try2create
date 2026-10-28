#include "tag_generator.h"
#include <algorithm>
#include <random>

namespace {

const std::vector<TagCategory>& BuildCategories() {
    static const std::vector<TagCategory> categories = {
        {"Subject count",
         {"1girl", "1boy", "2girls", "2boys", "3girls", "multiple_girls", "multiple_boys", "no_humans", "solo",
          "duo", "crowd"},
         true},

        {"Hair color",
         {"black_hair", "brown_hair", "blonde_hair", "silver_hair", "white_hair", "blue_hair", "red_hair",
          "pink_hair", "purple_hair", "green_hair", "orange_hair", "grey_hair", "multicolored_hair", "gradient_hair",
          "two-tone_hair"},
         true},

        {"Hair style",
         {"long_hair", "short_hair", "medium_hair", "very_long_hair", "twintails", "ponytail", "side_ponytail",
          "braid", "twin_braids", "bob_cut", "hair_bun", "double_bun", "messy_hair", "curly_hair", "straight_hair",
          "wavy_hair", "ahoge", "hime_cut", "bangs", "swept_bangs"},
         true},

        {"Eyes",
         {"blue_eyes", "red_eyes", "green_eyes", "brown_eyes", "purple_eyes", "yellow_eyes", "grey_eyes",
          "heterochromia", "glowing_eyes", "half_closed_eyes", "narrowed_eyes", "sparkling_eyes"},
         true},

        {"Expression",
         {"smile", "smirk", "blush", "closed_eyes", "open_mouth", "serious", "crying", "laughing", "angry",
          "surprised", "gentle_smile", "expressionless", "embarrassed", "determined", "sad", "shy"},
         true},

        {"Clothing",
         {"school_uniform", "sailor_collar", "dress", "kimono", "yukata", "armor", "swimsuit", "sundress", "hoodie",
          "sweater", "coat", "cape", "gloves", "thighhighs", "skirt", "shorts", "jacket", "suit", "apron",
          "off_shoulder", "turtleneck", "bodysuit", "robe", "cardigan", "hat", "beret", "ribbon", "necktie",
          "bowtie", "scarf"},
         true},

        {"Pose / action",
         {"standing", "sitting", "lying_down", "running", "jumping", "looking_at_viewer", "looking_back",
          "from_behind", "from_above", "from_side", "arms_up", "crossed_arms", "hand_on_hip", "walking", "reaching",
          "leaning_forward", "dancing", "floating", "flying", "stretching"},
         true},

        {"Setting / background",
         {"outdoors", "indoors", "forest", "city", "night", "day", "sunset", "cherry_blossoms", "beach", "snow",
          "rain", "clouds", "starry_sky", "classroom", "bedroom", "garden", "mountain", "ocean", "street", "ruins",
          "library", "cafe", "rooftop", "shrine", "castle", "meadow", "waterfall", "autumn_leaves"},
         true},

        {"Lighting / effects",
         {"dramatic_lighting", "backlighting", "rim_light", "glowing", "sparkle", "lens_flare", "depth_of_field",
          "bokeh", "volumetric_lighting", "motion_blur", "particles", "god_rays", "reflection", "shadow",
          "silhouette", "chromatic_aberration"},
         false},

        {"Quality / style boosters",
         {"masterpiece", "best_quality", "highly_detailed", "ultra_detailed", "official_art", "illustration",
          "digital_painting", "concept_art", "sharp_focus", "intricate_details", "cinematic_lighting", "4k",
          "absurdres", "extremely_detailed", "official_wallpaper"},
         true},
    };
    return categories;
}

} // namespace

const std::vector<TagCategory>& GetTagCategories() {
    return BuildCategories();
}

std::vector<std::string> GenerateRandomTags(const std::set<std::string>& enabledCategories,
                                            const std::vector<std::string>& extraTags, int totalCount) {
    std::vector<std::string> pool;
    for (const auto& cat : GetTagCategories()) {
        if (enabledCategories.count(cat.name)) {
            pool.insert(pool.end(), cat.tags.begin(), cat.tags.end());
        }
    }
    pool.insert(pool.end(), extraTags.begin(), extraTags.end());
    if (pool.empty() || totalCount <= 0) return {};

    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(pool.begin(), pool.end(), rng);

    int n = std::min(totalCount, static_cast<int>(pool.size()));
    return std::vector<std::string>(pool.begin(), pool.begin() + n);
}
