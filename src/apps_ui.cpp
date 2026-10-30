// Edits > Apps: one-click picture recipes in categories, each a fixed Qwen Image 2.1 Edit instruction (with a choice or a few words of yours)
// - relight, a new angle, expand, outfit swap, headshot, plushie... Asked for 2026-10-23, from the "Open Sourcing Higgsfield" map (its Apps
// hub). A card shows the last picture you made with it; the results also go to Picture edits as usual.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <windows.h>
#include <commdlg.h>

#include "comfy.h"
#include "imgui.h"
#include "json.hpp"
#include "ui_extra.h"
#include "ui_theme.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// One app. {X} in the instruction is your choice (choices: "Label=text|Label=text") or your words (ask). canvas: 0 the picture itself is
// edited (its frame kept), 1 landscape, 2 square, 3 portrait (a new canvas, the picture a reference). twoPictures: a second picture is needed.
// canvasChoice: the choice is the canvas (Expand). shots: one edit per choice, all at once (Shots).
struct App {
    const char* name;
    const char* category;
    const char* about;
    const char* instruction;
    const char* choices = nullptr;
    const char* ask = nullptr; // the label of a few words of yours
    bool twoPictures = false;
    bool canvasChoice = false;
    bool shots = false;
};

const char* kCategories[] = {"All", "Enhance & style", "Professional", "Face & identity", "Ads & products", "Games & characters", "Fun & retro", "Extras"};

const App kApps[] = {
    // ---- enhance and style
    {"Relight", "Enhance & style", "New light on the same picture: golden hour, studio, neon...",
     "Relight the picture with {X}. Keep the people, their faces, poses and clothes and the whole scene exactly the same; only the light, the shadows and the colour of the light change.",
     "Golden hour=warm low golden-hour sunlight from the side and long soft shadows|Studio=soft, even studio softbox light and clean soft shadows|Neon night=pink and blue neon light from the sides in a dark night|Candlelight=warm flickering candlelight and deep shadows|Overcast=soft overcast daylight with no harsh shadows|Rim light=a strong backlight that outlines the subject, the front darker|Moonlight=cool blue moonlight at night"},
    {"Skin enhancer", "Enhance & style", "A professional retouch: even skin, real texture kept",
     "Retouch the skin like a professional portrait retoucher: even out blemishes, redness and shine, keep natural pores and skin texture. Keep the face shape, features, expression, age, hair, clothes and background exactly the same."},
    {"Style snap", "Enhance & style", "The same picture redrawn in another style",
     "Redraw the whole picture as {X}, keeping the same composition, people, poses, expressions and colours where they fit.",
     "Anime=a clean modern anime illustration|3D animated=a 3D animated film still with soft lighting|Watercolour=a loose watercolour painting|Oil painting=a classical oil painting with visible brushstrokes|Comic book=a bold comic book panel with ink lines and halftone|Claymation=a claymation still, everything made of plasticine|Pencil sketch=a detailed pencil sketch on paper|Cyberpunk=a neon cyberpunk digital painting"},
    {"Outfit swap", "Enhance & style", "Dress them in something else (say what)",
     "Change the person's outfit to {X}. Keep their face, hair, body, pose, the light and the background exactly the same.", nullptr, "The new outfit"},
    {"Outfit from a picture", "Enhance & style", "The outfit worn in a second picture",
     "Dress the person in the first picture in the outfit worn in the second picture. Keep the first picture's person - face, hair, body, pose - and its background and light exactly the same.",
     nullptr, nullptr, true},
    {"AI stylist", "Enhance & style", "A new hairstyle and makeup (say which)",
     "Give the person {X}. Keep their face, identity, clothes, pose and the background the same.", nullptr, "Hair and makeup"},
    {"Colorize", "Enhance & style", "A black and white photo in natural colour",
     "Colorize this black and white photo with natural, realistic colours: skin, clothes, sky and surroundings as they would really be; keep every detail the same."},
    // (2026-10-29, after NVAI-open's photo restoration; theirs is FLUX 3: here the same edit as every app)
    {"Repair an old photo", "Enhance & style", "Scratches, dust, creases, stains and torn corners taken out",
     "Restore this old, damaged photograph: remove every scratch, speck of dust, crease, crack, water stain and fold, and fill in the torn or "
     "missing corner so it matches the rest of the picture. Keep the photograph itself exactly the same - the people, their faces, expressions "
     "and age, the clothes, the composition, the grain and the light - {X}.",
     "Keep its tones=and keep its own tones (a black and white or sepia photo stays as it is)|And colorize=and give it natural, realistic colours as they would really have been"},
    // ---- professional
    {"Expand", "Professional", "More of the scene around it, in a new shape",
     "Show more of the scene around the first picture: extend it naturally beyond its edges in the same style, light and perspective, keeping everything already in it exactly the same.",
     "Wider (16:9)=1|Square=2|Taller (9:16)=3", nullptr, false, true},
    {"New angle", "Professional", "The same scene from another camera angle",
     "Show the same scene with the same people {X}. Keep everyone's faces, hair, clothes and the place the same.",
     "From the side=seen from the side|From above=seen from high above, a bird's-eye view|Low angle=seen from a low angle looking up|From behind=seen from behind|Close-up=as a close-up of the face|Wide shot=as a wide shot showing the whole place|Over the shoulder=seen over the shoulder of the person"},
    {"Shots", "Professional", "Three framings at once: close-up, medium, wide",
     "Show the same scene with the same people {X}. Keep everyone's faces, hair, clothes and the place the same.",
     "Close-up=as a close-up of the face|Medium=as a medium shot from the waist up|Wide=as a wide shot showing the whole place", nullptr, false, false, true},
    {"Product shot", "Professional", "A studio product photo of the thing in it",
     "Turn this into a professional studio product photo: the product alone on a clean, seamless {X} background, soft studio lighting, crisp detail and a soft shadow beneath. Keep the product exactly the same.",
     "White=white|Pastel=soft pastel|Black=black|Marble=white marble|Wood=warm wooden"},
    // ---- face and identity
    {"Headshot", "Face & identity", "A professional headshot of the person",
     "Make a professional headshot of this person: {X}, shoulders up, looking at the camera with a friendly, confident expression. Keep their face and identity exactly the same.",
     "Business=business attire, soft studio light and a plain grey background|Creative=smart casual clothes, warm natural window light and a blurred office behind|Outdoor=smart casual clothes, outdoors in soft daylight with a blurred park behind"},
    {"Recast", "Face & identity", "The person from a second picture in their place",
     "Replace the person in the first picture with the person from the second picture, in the same pose, clothes, light and place. Keep everything else in the first picture exactly the same.",
     nullptr, nullptr, true},
    {"Age", "Face & identity", "Younger or older, the same person",
     "Make the person look {X}, keeping their identity, expression, clothes, pose and the background.",
     "Younger=clearly about fifteen years younger, with smoother skin|Older=clearly about twenty-five years older, with wrinkles, greying hair and an older face|As a child=like themselves as a child of about eight|Elderly=like themselves at about eighty, with white hair and deep wrinkles"},
    {"Expression", "Face & identity", "A new expression on the same face",
     "Change the person's expression to {X}. Keep everything else exactly the same.",
     "Big smile=a big, warm smile|Laughing=laughing out loud|Surprised=surprised, eyes wide|Angry=angry|Sad=sad, close to tears|Wink=a playful wink"},
    // ---- ads and products
    {"Billboard ad", "Ads & products", "The picture as a huge billboard in a city",
     "Show the first picture as a huge billboard advertisement on a building in a busy city street {X}, seen from the street with people walking by. The picture on the billboard stays exactly the same.",
     "Day=on a sunny day|Dusk=at dusk with the city lights coming on|Night=at night, the billboard lit up among neon signs"},
    {"Signboard", "Ads & products", "A sign in the scene with your words",
     "Put a sign in the scene that reads exactly: \"{X}\", in a style that fits the place (painted, neon, a shop sign...). Keep everything else the same.", nullptr, "The sign's words"},
    {"On a T-shirt", "Ads & products", "The picture printed on a T-shirt, worn",
     "Show the first picture printed on the front of a T-shirt worn by a model in a bright street-style photo; the print stays exactly the same."},
    // ---- games and characters
    {"Plushie", "Games & characters", "The subject as a cute plush toy",
     "Turn the main subject into a cute, soft plush toy with stitched seams and fuzzy fabric, sitting on a bed in cosy light; keep its colours and features recognisable."},
    {"Action figure", "Games & characters", "A collectible figure in its packaging",
     "Turn the person into a collectible action figure in a blister package with a few accessories beside it, a toy store product photo; keep their face, hair and clothes recognisable."},
    {"Game character", "Games & characters", "The person in a video game",
     "Show the person as a character in {X}, keeping their face, hair and clothes recognisable.",
     "Open world=a modern 3D open-world video game screenshot|Retro 16-bit=a retro 16-bit pixel art video game|Fighter=a fighting game character select screen|Cartoon game=a colourful cartoon platform game"},
    {"Sticker", "Games & characters", "A die-cut sticker",
     "Turn the main subject into a die-cut sticker: flat cartoon style, a thick white border around it, on a plain light grey background."},
    // ---- extras
    {"Background remover", "Extras", "Only the subject, on plain white",
     "Remove the background: keep only the main subject, unchanged, on a plain pure white background."},
    {"New background", "Extras", "The subject somewhere else (say where)",
     "Put the main subject in {X}, matching the light and perspective. Keep the subject exactly the same.", nullptr, "The new place"},
    {"Remove something", "Extras", "Take something out (say what)",
     "Remove {X} from the picture and fill the space in naturally. Keep everything else exactly the same.", nullptr, "What to remove"},
    {"Day to night", "Extras", "The same place at another time",
     "Show the same scene {X}, with the light, sky and colours that go with it. Keep everything in it the same.",
     "At night=at night with lights on|At sunset=at sunset|In winter=in winter with snow|In autumn=in autumn with orange leaves|In the rain=in heavy rain with wet reflections"},
    // ---- from Freefield's template apps (neuregex/freefield, MIT; 2026-10-28), their prompts worded for Qwen Image 2.1 Edit
    {"Color grade", "Enhance & style", "A film colour grade: teal and orange, noir, pastel...",
     "Colour grade this photo with {X}; keep every detail and the composition.",
     "Teal and orange=a strong teal and orange cinematic grade, shadows pushed to deep teal, skin tones and highlights warm orange, high contrast, slightly lifted blacks, fine film grain, like a blockbuster movie still|Warm vintage film=a warm vintage film grade, golden-brown midtones, faded blacks, soft contrast, a slight green in the shadows and fine grain|Cold noir=a cold noir grade, heavily desaturated, blue-black shadows, pale cool skin, crushed blacks, hard contrast, a faint cyan tint in the highlights|Pastel=a soft pastel grade, low contrast, lifted shadows, pale pink, mint and lavender tints, bright airy highlights|High-contrast B&W=a high-contrast black and white grade, no colour at all, deep blacks, bright whites, strong local contrast and visible grain|Golden hour=a golden hour grade, warm amber light over everything, warm highlights on the skin, a soft orange glow, gentle contrast"},
    {"Photo effects", "Enhance & style", "Glitch, double exposure, neon outline, infrared, thermal...",
     "Apply {X} to this photo; the scene stays recognizable.",
     "Glitch=a heavy digital glitch effect: strong horizontal displacement bands, red and cyan RGB channel split, blocky pixel corruption, scan lines and data-mosh smears across the frame|Double exposure=a double exposure: the silhouette of the main subject filled with a pine forest and mountains at dusk, the background faded to white, a soft fine-art blend|Neon outline=glowing neon outlines: bright pink and cyan neon tubes trace the edges of the subject and the main shapes, the rest of the photo darkened to near black|Pencil sketch=a graphite pencil sketch look: the whole photo redrawn as a detailed pencil drawing on white paper, hatching for the shadows, no colour|Watercolour=a watercolour painting look: soft washes of colour bleeding into each other, visible paper texture, loose brush edges, white paper showing through|Pop art=a pop art look: bold flat colours, thick black outlines and halftone dots, like a 1960s comic print|Vintage film=a vintage 1970s film photo look: faded warm colours, lifted blacks, heavy film grain, light leaks and a slight blur|Infrared=an infrared photo look: foliage and skin turned pale white and pink, dark skies, high contrast, a dreamy glow|Anaglyph 3D=a red-cyan anaglyph 3D look: the image doubled, a red copy shifted left and a cyan copy shifted right, like an old 3D-glasses print|Thermal camera=a thermal camera look: false colours from deep blue for cold to yellow and white for warm, people glowing orange and yellow, no fine texture"},
    {"Burning sunset", "Enhance & style", "The same scene under a blazing sunset sky",
     "The scene of the picture under a blazing sunset: the sky on fire with orange and crimson clouds, long shadows, warm light on everything, composition unchanged."},
    {"3D render", "Enhance & style", "The person as a 3D animated-film character",
     "Turn this photo into a stylized 3D character render like a still from a modern animated film: the same person as a 3D cartoon character with smooth skin, large expressive eyes, exaggerated proportions, matte clay-like materials, soft studio lighting with subsurface scattering, clean gradient background; the same hairstyle and clothes."},
    {"Sketch to photo", "Enhance & style", "A sketch or drawing turned into a real photo",
     "Turn the sketch in the picture into a photorealistic photograph of the same scene: the same composition and subjects, real materials, natural light."},
    {"Packshot", "Professional", "The closing shot of an ad: the product on a clean gradient",
     "The closing packshot of a commercial: the product from the picture centered on a smooth {X} gradient background, a soft shadow under it, generous empty space above it. Keep the product identical: shape, label, text and colours.",
     "Dark charcoal=dark charcoal|Clean white=clean white|Deep blue=deep blue|Warm beige=warm beige"},
    {"Macro detail", "Professional", "An extreme macro close-up of the product's texture",
     "An extreme macro photograph of a detail of the product from the picture: its texture, edges and label fill the frame, 100mm macro lens, soft diffused light. Keep the product identical: shape, label, text and colours."},
    {"Macro scene", "Professional", "The product in a macro scene: wet stone, leaves, sand, frost",
     "An extreme macro close-up of the product from the picture {X}, shallow depth of field, tiny water droplets, crisp texture. Keep the product identical: shape, label, text and colours.",
     "Wet black stone=on wet black stone with mist|Green leaves=among fresh green leaves|Sand at golden hour=on sand at golden hour|Ice and frost=on ice with frost crystals"},
    {"Phone snapshot", "Professional", "A casual UGC phone shot: a hand holding the product",
     "Add a hand holding this product from below and replace the background with {X}, as a casual phone snapshot with slightly tilted framing and natural light. The product itself does not change: the same angle, the same size, and the label text stays sharp and perfectly legible, exactly as written in the photo.",
     "Bright kitchen=a bright kitchen|Cafe table=a busy cafe|Outdoors in a park=a sunny park"},
    {"Knolling", "Professional", "The subject taken apart and laid out neatly, top-down",
     "The subject of the picture taken apart into its components, laid out neatly on a grid with small labels, knolling photography, top-down, white background."},
    {"Fashion catalog", "Professional", "The person wearing the garment from a second picture",
     "A fashion catalog photo: the person from the first picture wearing the garment from the second picture, full-length, {X}. Keep the face identical and the garment's cut, colour and pattern exactly as in its photo.",
     "Studio cyclorama=in a white studio cyclorama|City street, golden hour=on a city street at golden hour|Minimalist loft=in a minimalist loft|Runway=walking a runway under spotlights", nullptr, true},
    {"Face swap", "Face & identity", "The face from a second picture on this person",
     "Give the person in the first picture exactly the face of the person in the second picture: the same eyes, nose, mouth, brows and face shape. Keep the first picture's own hairline and hair; match its head angle, expression, lighting and skin tone.",
     nullptr, nullptr, true},
    {"Commercial faces", "Face & identity", "The person in a campaign shot: sportswear, skincare, corporate",
     "Turn this photo into {X} Keep the face identical. No text, letters or logos anywhere in the image.",
     "Sportswear=a sportswear campaign shot: the same person in a black athletic top, a light sheen of sweat, dramatic low-key lighting with a rim light, dark concrete gym background, intense look at the camera, editorial composition.|Skincare advert=a skincare advertisement: the same person in a soft white top with glowing dewy skin, holding a small white jar of face cream near the cheek, bright white studio with soft beauty-dish lighting, clean pastel backdrop, glossy magazine composition.|Corporate brochure=a corporate brochure portrait: the same person in a tailored navy suit, standing in a bright glass office with a blurred city view behind, soft window light, confident friendly expression, crisp professional photography.|Fashion editorial=a fashion editorial shot: the same person in a structured designer coat, dramatic side light carving the face, a seamless coloured paper backdrop, a strong pose, high-end magazine photography."},
    {"Renaissance", "Face & identity", "The person as a Renaissance oil painting in a gilded frame",
     "The person from the picture as a Renaissance oil painting: chiaroscuro light, period clothing, cracked varnish, in an ornate gilded frame on a museum wall. Keep the face, hairstyle and build identical."},
    {"Fridge ad", "Ads & products", "The product in a lit glass-door shop fridge",
     "The product from the picture standing inside an illuminated glass-door display fridge in a shop, cold light, condensation on the glass, rows of shelves. Keep the product identical: shape, label, text and colours."},
    {"Giant product", "Ads & products", "The product as a giant monument over a city",
     "The product from the picture as a giant monument standing over a city skyline {X}, people and cars tiny at its base, wide-angle shot, realistic scale and lighting. Keep the product identical: shape, label, text and colours.",
     "Sunset=at sunset|Night, city lights=at night with the city lights on|Foggy morning=on a foggy morning"},
    {"Graffiti ad", "Ads & products", "The product as a spray-paint mural on a brick wall",
     "The product from the picture painted as a large spray-paint graffiti mural on a brick wall in the street, paint drips and stencil edges, daylight. Keep its shape, label and colours recognizable."},
    {"Ad poster", "Ads & products", "A printed ad poster for the product, with your headline",
     "A printed advertising poster for the product from the picture: the product large in the center, a bold headline reading \"{X}\" at the top, a clean typographic layout, studio lighting, vivid colours. Keep the product identical: shape, label, text and colours.",
     nullptr, "The headline"},
    {"Outfit vending", "Ads & products", "The clothes worn in the picture inside a neon vending machine",
     "The clothes and accessories worn in the picture displayed as separate items inside an illuminated glass vending machine at night, each in its own slot with a price tag, neon glow."},
    {"Comic book", "Fun & retro", "A comic book panel: ink, halftone, a caption box",
     "The photo as a comic book panel: bold ink outlines, halftone dots, flat colours, an action caption box, dynamic framing."},
    {"60s cafe", "Fun & retro", "The person in a 1960s cafe, Kodachrome look",
     "The person from the picture sitting in a 1960s cafe: period clothes and hairstyle, chrome counter, pastel colours, Kodachrome film grain, warm light. Keep the face, hairstyle and build identical."},
    {"Giallo", "Fun & retro", "A 1970s Italian giallo film still, red and blue gels",
     "The person from the picture in a 1970s Italian giallo film still: saturated red and blue gel lighting, deep shadows, a gloved hand at the edge of the frame, film grain. No blood, no wounds. Keep the face, hairstyle and build identical."},
    {"Japanese magazine", "Fun & retro", "The person on a Japanese fashion magazine cover",
     "The person from the picture on the cover of a Japanese fashion magazine: bold vertical Japanese headlines, a masthead at the top, colourful text blocks, glossy print look. Keep the face, hairstyle and build identical."},
    {"Japanese poster", "Fun & retro", "A bold red and black Japanese-style poster",
     "A Japanese-style poster featuring the person from the picture: bold red and black graphic design, vertical typography, rising-sun rays, print texture. Keep the face, hairstyle and build identical."},
    {"Japanese TV show", "Fun & retro", "The person on a retro Japanese variety show",
     "The person from the picture on a retro Japanese TV variety show: a 4-panel screen layout, bright studio set, colourful caption text overlays, 1990s broadcast look. Keep the face, hairstyle and build identical."},
    {"90s paint program", "Fun & retro", "Redrawn as a crude 1990s computer paint drawing",
     "The photo redrawn as a crude drawing made in a 1990s computer paint program: aliased lines, flat fill colours, a spray-can texture, a white canvas with the program's toolbar at the edge."},
    {"Pixel game", "Fun & retro", "The scene as 16-bit pixel art with a game HUD",
     "The scene of the picture redrawn as 16-bit pixel art from a retro video game, limited palette, visible pixels, a side-view game HUD at the top."},
    {"Street mural", "Fun & retro", "The person hand-painted on a building's wall",
     "The person from the picture hand-painted on a large street mural on the side of a building, visible brush strokes, weathered paint, pedestrians passing below."},
    {"Playing card", "Fun & retro", "The person as the queen of hearts on casino felt",
     "Make one large ornate playing card, the queen of hearts, lying on green casino felt next to a stack of poker chips. The card's portrait is this person's face, large and clearly recognizable, drawn in the classic playing-card illustration style with gold foil borders and the Q and heart symbols in the corners."},
    {"Micro beasts", "Fun & retro", "Tiny cute animals climbing on the person",
     "The person from the picture surrounded by tiny cute animals: {X} climbing on their shoulders and hands, sharp detail, soft daylight. Keep the face, hairstyle and build identical.",
     "Hamsters and kittens=hamsters and kittens|Ducklings and bunnies=ducklings and bunnies|Tiny puppies=tiny puppies|Baby otters=baby otters"},
    {"Mascot", "Fun & retro", "The person in a football club mascot costume",
     "The person from the picture in a full football club mascot costume: an oversized foam head of a {X} held under one arm, team colours, standing on a stadium pitch, the crowd behind. The kit is a plain unbranded club kit with only a simple club crest: no sportswear brand logos, stripes or marks. Keep the face, hairstyle and build identical.",
     "Lion=lion|Eagle=eagle|Bulldog=bulldog|Bear=bear"},
    {"Brick toy", "Games & characters", "The subject rebuilt from plastic toy bricks",
     "The subject of the picture rebuilt as a toy made of interlocking plastic bricks, blocky shapes, studs visible, on a white table."},
    {"Glitter sticker", "Games & characters", "A cute holographic glitter sticker of the person",
     "The person from the picture as a glossy glitter sticker: a simplified cute illustration, sparkling holographic texture, a thick white die-cut border, on a plain white background."},
};
constexpr int kAppCount = static_cast<int>(sizeof(kApps) / sizeof(kApps[0]));

struct Choice {
    std::string label, text;
};
std::vector<Choice> ChoicesOf(const App& a) {
    std::vector<Choice> out;
    if (!a.choices) return out;
    const std::string s = a.choices;
    size_t at = 0;
    while (at <= s.size()) {
        const size_t bar = s.find('|', at);
        const std::string item = s.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
        const size_t eq = item.find('=');
        out.push_back({item.substr(0, eq), eq == std::string::npos ? item : item.substr(eq + 1)});
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    return out;
}

struct Page {
    bool loaded = false;
    int category = 0;
    int selected = 0;
    std::wstring first, second;
    int choice = 0;
    char words[200] = {};
    int times = 1;
    std::map<std::string, std::wstring> lastMade; // by app: its newest result (the card's picture)
    std::vector<std::pair<int, std::string>> waiting; // activity -> app
    std::vector<std::pair<std::string, std::wstring>> made; // this session, newest last (app, picture)
} g_a;

std::wstring SavePath() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Try2Create\\apps.json";
}

void Load() {
    if (g_a.loaded) return;
    g_a.loaded = true;
    std::ifstream f(SavePath(), std::ios::binary);
    const json j = f ? json::parse(f, nullptr, false) : json();
    if (!j.is_object() || !j.contains("lastMade") || !j["lastMade"].is_object()) return;
    std::error_code ec;
    for (auto it = j["lastMade"].begin(); it != j["lastMade"].end(); ++it)
        if (it.value().is_string() && fs::exists(W(it.value().get<std::string>()), ec)) g_a.lastMade[it.key()] = W(it.value().get<std::string>());
}

void Save() {
    json m = json::object();
    for (const auto& [k, v] : g_a.lastMade) m[k] = U8(v);
    const std::string text = json({{"lastMade", m}}).dump(1, ' ', false, json::error_handler_t::replace);
    std::ofstream(SavePath(), std::ios::binary | std::ios::trunc) << text;
}

void Pump(AppState& app) {
    if (g_a.waiting.empty() || !app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    for (size_t i = 0; i < g_a.waiting.size();) {
        bool done = false;
        for (const ComfyActivity& a : acts) {
            if (a.id != g_a.waiting[i].first) continue;
            if (a.state == ComfyState::Done) {
                for (const ComfyResult& r : a.files) {
                    if (r.localPath.empty() || r.video || r.audio) continue;
                    g_a.lastMade[g_a.waiting[i].second] = r.localPath;
                    g_a.made.push_back({g_a.waiting[i].second, r.localPath});
                }
                Save();
                done = true;
            } else if (a.state == ComfyState::Failed) {
                done = true;
            }
        }
        if (done) g_a.waiting.erase(g_a.waiting.begin() + i);
        else ++i;
    }
}

std::wstring PickPicture() {
    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.webp;*.bmp\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"A picture";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

// A picture (blurred when NSFW blur says so) fitted into w x h; true when clicked
bool Picture(AppState& app, const std::wstring& path, float w, float h, const char* id) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool click = ImGui::InvisibleButton(id, ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 12), ui::Dp(6.0f));
    if (!path.empty()) {
        const AppState::LibThumb lt = app.LibraryThumb(path);
        if (lt.srv && lt.w > 0 && lt.h > 0) {
            const float s = std::max(w / lt.w, h / lt.h); // (filled: cropped to the card)
            const float uw = std::min(1.0f, w / (lt.w * s)), vh = std::min(1.0f, h / (lt.h * s));
            dl->AddImageRounded((ImTextureID)(intptr_t)lt.srv, p, ImVec2(p.x + w, p.y + h), ImVec2((1 - uw) * 0.5f, (1 - vh) * 0.5f), ImVec2((1 + uw) * 0.5f, (1 + vh) * 0.5f),
                                IM_COL32_WHITE, ui::Dp(6.0f));
        }
    }
    return click;
}

// One picture slot: its picture, Gallery's / Newest / File...
void Slot(AppState& app, const char* label, std::wstring& path, const char* id) {
    ImGui::BeginGroup();
    ImGui::PushID(id);
    ImGui::TextDisabled("%s", label);
    const float s = ui::Dp(150.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    Picture(app, path, s, s * 1.2f, "##pic");
    if (ImGui::BeginDragDropTarget()) { // (a picture dragged here from the gallery or the Vision library)
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MV_LIBITEM")) path = static_cast<const wchar_t*>(pl->Data);
        else if (const ImGuiPayload* pg = ImGui::AcceptDragDropPayload("MV_IMAGES")) {
            const int idx = *static_cast<const int*>(pg->Data);
            if (idx >= 0 && idx < static_cast<int>(app.entries.size())) path = app.entries[idx].path;
        }
        ImGui::EndDragDropTarget();
    }
    if (path.empty()) {
        const char* t = "none yet";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (s - ts.x) * 0.5f, p.y + (s * 1.2f - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", U8(fs::path(path).filename().wstring()).c_str());
    }
    const bool sel = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size());
    ImGui::BeginDisabled(!sel);
    if (ImGui::SmallButton("Gallery's")) path = app.entries[app.selectedIndex].path;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(sel ? "The picture selected in the gallery" : "Select a picture in the gallery first");
    ImGui::SameLine();
    if (ImGui::SmallButton("Newest")) {
        std::wstring newest;
        if (app.comfySvc)
            for (const ComfyResult& r : app.comfySvc->Results())
                if (!r.localPath.empty() && !r.audio && !r.video) newest = r.localPath;
        if (!newest.empty()) path = newest;
        else app.SetStatus("Nothing made yet this session");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The newest picture ComfyUI made this session");
    ImGui::SameLine();
    if (ImGui::SmallButton("File...")) {
        const std::wstring f = PickPicture();
        if (!f.empty()) path = f;
    }
    ImGui::PopID();
    ImGui::EndGroup();
}

void Make(AppState& app, const App& a) {
    const std::vector<Choice> choices = ChoicesOf(a);
    std::vector<std::wstring> pics = {g_a.first};
    if (a.twoPictures) pics.push_back(g_a.second);
    auto send = [&](const std::string& fill, int canvas, const std::string& label) {
        std::string instruction = a.instruction;
        const size_t at = instruction.find("{X}");
        if (at != std::string::npos) instruction.replace(at, 3, fill);
        EditContext ctx;
        ctx.canvas = canvas;
        if (SendPictureEdit(app, pics, instruction, label, a.shots ? 1 : g_a.times, &ctx)) g_a.waiting.push_back({LastEditActivity(), a.name});
    };
    if (a.shots) { // every framing, at once
        for (const Choice& c : choices) send(c.text, 0, std::string(a.name) + ": " + c.label);
    } else if (a.canvasChoice) {
        const Choice& c = choices[std::clamp(g_a.choice, 0, static_cast<int>(choices.size()) - 1)];
        send(std::string(), std::atoi(c.text.c_str()), std::string(a.name) + ": " + c.label);
    } else if (!choices.empty()) {
        const Choice& c = choices[std::clamp(g_a.choice, 0, static_cast<int>(choices.size()) - 1)];
        send(c.text, 0, std::string(a.name) + ": " + c.label);
    } else {
        send(g_a.words, 0, std::string(a.name) + (a.ask ? std::string(": ") + g_a.words : std::string()));
    }
    app.requestTab = 6; // (stay here: SendPictureEdit may have asked for the Edits page, which this is)
}

} // namespace

void DrawAppsBody(AppState& app, float height) {
    Load();
    Pump(app);
    { // tests: MV_APP_TEST="App name|picture[|choice]" picks it and makes it once
        static bool once = false;
        wchar_t buf[1024] = {};
        if (!once && GetEnvironmentVariableW(L"MV_APP_TEST", buf, 1024) > 0) {
            once = true;
            const std::string spec = U8(buf);
            const size_t bar = spec.find('|'), bar2 = spec.find('|', bar + 1);
            for (int i = 0; i < kAppCount; ++i)
                if (bar != std::string::npos && spec.substr(0, bar) == kApps[i].name) g_a.selected = i;
            g_a.first = W(spec.substr(bar + 1, bar2 == std::string::npos ? std::string::npos : bar2 - bar - 1));
            g_a.choice = bar2 == std::string::npos ? 0 : std::atoi(spec.c_str() + bar2 + 1);
            Make(app, kApps[g_a.selected]);
        }
    }
    if (!ImGui::BeginTable("##apps", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, height))) return;
    ImGui::TableSetupColumn("cards", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    ImGui::TableSetupColumn("app", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableNextRow(0, height);
    // ---- the cards
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginChild("##appcards", ImVec2(0, height));
    for (int c = 0; c < IM_ARRAYSIZE(kCategories); ++c) {
        if (c) ui::SameLineIfFits(kCategories[c]);
        ImGui::PushStyleColor(ImGuiCol_Button, g_a.category == c ? ui::ChipActiveColor() : ui::ChipColor());
        if (ImGui::SmallButton(kCategories[c])) g_a.category = c;
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    const float cw = ui::Dp(132.0f), ch = ui::Dp(150.0f), gap = ui::Dp(10.0f);
    const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (cw + gap)));
    int shown = 0;
    for (int i = 0; i < kAppCount; ++i) {
        const App& a = kApps[i];
        if (g_a.category != 0 && std::string(a.category) != kCategories[g_a.category]) continue;
        if (shown++ % cols) ImGui::SameLine(0, gap);
        ImGui::BeginGroup();
        ImGui::PushID(i);
        auto last = g_a.lastMade.find(a.name);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool click = Picture(app, last != g_a.lastMade.end() ? last->second : std::wstring(), cw, ch, "##card");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (last == g_a.lastMade.end()) { // none made yet: its name and what it does, in the box
            // (the name at a size that fits in two lines, the words under wherever it ends: a two-line name ran into them)
            ImFont* tf = ui::TitleFont();
            float fs = tf->FontSize;
            while (fs > ImGui::GetFontSize() * 1.15f && tf->CalcTextSizeA(fs, FLT_MAX, cw - ui::Dp(20.0f), a.name).y > fs * 2.3f) fs -= 1.0f;
            const float nameH = tf->CalcTextSizeA(fs, FLT_MAX, cw - ui::Dp(20.0f), a.name).y;
            dl->AddText(tf, fs, ImVec2(p.x + ui::Dp(10.0f), p.y + ui::Dp(10.0f)), ImGui::GetColorU32(ImGuiCol_Text), a.name, nullptr, cw - ui::Dp(20.0f));
            dl->AddText(nullptr, 0.0f, ImVec2(p.x + ui::Dp(10.0f), p.y + std::max(ch * 0.40f, ui::Dp(16.0f) + nameH)), ImGui::GetColorU32(ImGuiCol_TextDisabled), a.about, nullptr,
                        cw - ui::Dp(20.0f));
        } else { // its name over the picture
            const ImVec2 ts = ImGui::CalcTextSize(a.name);
            dl->AddRectFilled(ImVec2(p.x, p.y + ch - ts.y - ui::Dp(10.0f)), ImVec2(p.x + cw, p.y + ch), IM_COL32(0, 0, 0, 160), ui::Dp(6.0f), ImDrawFlags_RoundCornersBottom);
            dl->AddText(ImVec2(p.x + ui::Dp(8.0f), p.y + ch - ts.y - ui::Dp(5.0f)), IM_COL32(255, 255, 255, 240), a.name);
        }
        if (g_a.selected == i) dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), ImGui::GetColorU32(ui::kAccent), ui::Dp(6.0f), 0, ui::Dp(2.5f));
        else if (ImGui::IsItemHovered()) dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), ImGui::GetColorU32(ui::kAccentHover), ui::Dp(6.0f), 0, ui::Dp(1.5f));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s\n%s", a.name, a.about);
        if (click && g_a.selected != i) g_a.selected = i, g_a.choice = 0;
        ImGui::PopID();
        ImGui::EndGroup();
    }
    ImGui::EndChild();
    // ---- the app picked
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("##appform", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    const App& a = kApps[std::clamp(g_a.selected, 0, kAppCount - 1)];
    ImGui::PushFont(ui::TitleFont());
    ImGui::TextUnformatted(a.name);
    ImGui::PopFont();
    ImGui::TextDisabled("%s  -  %s", a.category, a.about);
    ImGui::Spacing();
    Slot(app, "The picture", g_a.first, "first");
    if (a.twoPictures) {
        ImGui::SameLine(0, ui::Dp(16.0f));
        Slot(app, std::string(a.name) == "Recast" ? "Who goes in" : "The outfit's picture", g_a.second, "second");
    }
    const std::vector<Choice> choices = ChoicesOf(a);
    if (!choices.empty()) {
        ui::SectionHeading(a.shots ? "THE SHOTS (ALL MADE)" : a.canvasChoice ? "THE NEW SHAPE" : "WHICH");
        for (size_t c = 0; c < choices.size(); ++c) {
            if (c) ui::SameLineIfFits(choices[c].label.c_str());
            const bool on = a.shots || g_a.choice == static_cast<int>(c);
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
            if (ImGui::SmallButton(choices[c].label.c_str()) && !a.shots) g_a.choice = static_cast<int>(c);
            ImGui::PopStyleColor();
            if (!a.canvasChoice && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", choices[c].text.c_str());
        }
    }
    if (a.ask) {
        ui::SectionHeading(a.ask);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##words", g_a.words, sizeof(g_a.words));
    }
    if (!a.shots) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("How many:");
        for (int t = 1; t <= 4; ++t) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, g_a.times == t ? ui::ChipActiveColor() : ui::ChipColor());
            if (ImGui::SmallButton(std::to_string(t).c_str())) g_a.times = t;
            ImGui::PopStyleColor();
        }
    }
    const bool ready = !g_a.first.empty() && (!a.twoPictures || !g_a.second.empty()) && (!a.ask || g_a.words[0]);
    ImGui::Spacing();
    ImGui::BeginDisabled(!ready);
    char go[64];
    snprintf(go, sizeof(go), a.shots ? "Make the %d shots" : "Make it", static_cast<int>(choices.size()));
    if (ui::PrimaryButton(go, ImVec2(ui::Dp(170.0f), ui::Dp(36.0f)))) Make(app, a);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s, about %s each", app.comfy.editEngine == "flux3" && Flux3EditReady() ? "FLUX 3 Image Klein" : "Qwen Image 2.1 Edit", FormatEditTime(EstimateEditSeconds(app.comfy.editSteps, app.comfy.editResolution, a.twoPictures ? 2 : 1, 0)).c_str());
    if (!ready) ImGui::TextDisabled(g_a.first.empty() ? "Pick the picture first." : a.twoPictures && g_a.second.empty() ? "Pick the second picture too." : "Say it in a few words first.");
    const int making = static_cast<int>(std::count_if(g_a.waiting.begin(), g_a.waiting.end(), [&](const auto& w) { return w.second == a.name; }));
    if (making) ImGui::TextColored(ui::kAccent, "Making %d...", making);
    // what it made this session
    std::vector<std::wstring> mine;
    for (const auto& [n, pth] : g_a.made)
        if (n == a.name) mine.push_back(pth);
    if (!mine.empty()) {
        ui::SectionHeading("MADE WITH IT (ALSO IN PICTURE EDITS)");
        const float tw = ui::Dp(150.0f);
        for (size_t k = mine.size(); k-- > 0;) {
            if (k + 1 != mine.size()) ui::SameLineIfFits("XXXXXXXXXXXXXXXXX");
            ImGui::PushID(static_cast<int>(k));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (Picture(app, mine[k], tw, tw * 1.25f, "##made")) app.OpenFile(mine[k]), app.requestTab = 0;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Open it in the gallery\nRight-click: use it as this app's picture, mark it NSFW");
            if (ImGui::BeginPopupContextItem("##madectx")) {
                if (ImGui::MenuItem("Use it as the picture (do more to it)")) g_a.first = mine[k];
                NsfwMarkMenu(app, mine[k]);
                ImGui::EndPopup();
            }
            DrawNsfwOverlay(app, mine[k], p, ImVec2(p.x + tw, p.y + tw * 1.25f));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndTable();
}

bool AppsTakePictures(const std::vector<std::wstring>& paths) { // (dropped on the page: the first picture, a second for two-picture apps)
    if (paths.empty()) return false;
    g_a.first = paths[0];
    if (paths.size() > 1) g_a.second = paths[1];
    return true;
}
