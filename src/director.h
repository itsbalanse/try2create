#pragma once
// The "director": the model that edits pictures only really changes clothes, pose and expression when it is told WHAT to change, concretely ("wears a mustard
// yellow rain jacket, laughs with her eyes shut"), and ignores permission such as "feel free to re-pose". So for every character the director picks a concrete
// outfit, body posture and expression that fit the scene, and the edit instruction says so. The picks are made by Clef (a decision model) when it is running,
// otherwise at random from built-in lists. The Clef mode (fun mode) uses the same picks to invent whole scenarios again and again.
#include <string>
#include <vector>

struct CharLook {
    std::string outfit, pose, expression;
};

struct DirectorChar {
    std::string name, description; // description: what the card says about them ("a tall bearded man with glasses")
    std::string tag;               // "<image2>" in the instruction
};

struct DirectorFlags {
    bool outfit = true, pose = true, expression = true; // which of the three to direct
    bool adaptOutfit = false;                           // keep each character's own outfit and style, only adapt it to the scene (the "fit everything" style)
};

// The lists Clef chooses genres, moods and eras from (also used to tag the Vision library).
const std::vector<std::string>& GenreNames();
const std::vector<std::string>& MoodNames();
const std::vector<std::string>& EraNames(); // "the present day", "medieval"...

// Concrete looks for the characters, fitting `context` (what is said about the place, weather, mood and action). `clefUsed` tells whether Clef made the choices.
// outfitList: outfits to choose from instead of the modern ones (an era's costumes).
std::vector<CharLook> DirectLooks(const std::vector<DirectorChar>& chars, const std::string& context, const DirectorFlags& flags, bool useClef, bool* clefUsed = nullptr,
                                  const std::vector<std::string>* outfitList = nullptr);
// "Directions for each character (...): Name (<image2>) wears ...; posture: ...; expression: ..." ("" when nothing is directed)
std::string LooksClause(const std::vector<DirectorChar>& chars, const std::vector<CharLook>& looks, const DirectorFlags& flags);
// One random concrete look for a single picture ("wears ..., posture ..., expression ...") for the quick "Surprise me" button.
std::string RandomLookSentence(const std::string& description, const std::string& context);

// ---------------------------------------------------------------------------------------------------------------
// Clef mode: a whole scenario invented from choices.
struct Scenario {
    std::vector<int> cast;                // indices into the pool of characters
    std::string place, time, mood, twist; // "a flooded library", "at dawn, thick fog", "an epic action movie still", "confetti falls from the ceiling"
    std::string era, eraPhrase, region, genre; // "medieval", "in the Middle Ages", "Paris, France", "a heist caper"
    std::string prop, photoStyle, beat;     // "an old brass lantern", "shot on 35mm film...", "an argument is brewing" (story mode)
    double score = -1;                      // how good the idea looked to Clef when it judged the candidates (-1 = not judged)
    std::vector<std::string> relations;   // relations[i]: cast[i] <relation> cast[i + 1] (cast[0] when there is only one: empty)
    int shot = 0, angle = 0, lighting = 0; // indices into CameraChoices(0 / 1 / 4), 0 = auto
    int canvas = 1;                       // the edit's "shape of the result" (1..6)
    std::vector<CharLook> looks;          // per cast member
    bool clefUsed = false;
    std::string note;                     // "Clef chose" / "random (Clef is not running)"
    std::string Summary(const std::vector<DirectorChar>& pool) const; // one line for the list
    std::string PlaceText() const;                                    // "a castle courtyard, in the Middle Ages" / "a rainy street in Tokyo, Japan"
};

// relationLabels: what two characters can be doing (the Vision board's relation library); empty = built-in ones.
// bestOf > 1 and Clef: that many candidate scenarios are invented and Clef JUDGES them (a "how good is this idea" question for each) and the best is kept.
// story: the earlier scenarios of this run (their summaries); each new one is chosen as the next scene of that story, with the cast carried over.
struct StoryState {
    std::vector<std::string> summaries; // the last scenes
    std::vector<int> lastCast;          // pool indices of the previous scene's characters
    std::string lastEra;                // the era of the previous scene (a story stays in its world)
    int step = 0;                       // how many scenes so far
};
// How well each pair of characters works together in a picture, judged by Clef from their names and descriptions (0 = they clash ... 3 = a great duo).
// m[i][j] is symmetric; empty when Clef cannot answer. Cached by description, so asking again is free.
std::vector<std::vector<double>> ComputeChemistry(const std::vector<DirectorChar>& pool);

Scenario PlanScenario(const std::vector<DirectorChar>& pool, int minChars, int maxChars, const std::vector<std::string>& relationLabels, const DirectorFlags& flags, bool useClef, unsigned seed,
                      int bestOf = 1, StoryState* story = nullptr, const std::vector<std::vector<double>>* chemistry = nullptr);
