#include "director.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <numeric>
#include <random>

#include "board.h"
#include "clef.h"
#include "json.hpp"

using json = nlohmann::json;

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool Has(const std::string& low, std::initializer_list<const char*> words) {
    for (const char* w : words) {
        if (low.find(w) != std::string::npos) return true;
    }
    return false;
}

// ---------------------------------------------------------------- the options
const std::vector<std::string> kRain = {
    "a mustard yellow hooded rain jacket over a grey sweater, dark jeans and waterproof boots",
    "a long black raincoat with the collar up, dark trousers and leather boots",
    "a clear plastic poncho over a bright hoodie, shorts and rain boots",
    "a navy peacoat, a thick wool scarf, jeans and chelsea boots",
    "a forest green parka with the hood up, black joggers and white sneakers",
    "a red trench coat belted at the waist, a patterned scarf and ankle boots",
    "an oversized denim jacket, a white tee, cargo pants and chunky boots, hair damp from the rain",
    "a silver puffer jacket, a beanie, slim jeans and sneakers, holding a small umbrella",
    "a bright blue waterproof shell jacket over a hoodie, cargo trousers and trail shoes, a cap keeping the rain off",
    "a long olive waxed coat, a flat cap, wool trousers and sturdy boots",
    "a pink translucent rain cape over a cosy knit, jeans and shiny rain boots",
    "a black leather jacket with the collar up, a dark scarf, slim trousers and boots, a clear umbrella in hand",
    "a checked flannel shirt under a rubberised work jacket, jeans and gumboots",
    "a charcoal duffle coat with toggles, a bobble hat, corduroy trousers and sneakers"};
const std::vector<std::string> kCold = {
    "a thick cream knit sweater, a puffy down coat, a bobble hat, gloves and winter boots",
    "a long charcoal wool coat, a cashmere scarf, leather gloves and black boots",
    "a bright orange ski jacket, a beanie, goggles pushed up on the forehead and snow pants",
    "a fluffy shearling-lined jacket, a chunky scarf, jeans and fur-lined boots",
    "a burgundy puffer coat with a big hood, a striped scarf, mittens and boots",
    "a camel overcoat over a turtleneck, a felt hat and warm gloves",
    "a patterned Nordic knit cardigan, a woollen cap, thick socks and sturdy boots",
    "a black leather jacket with a hoodie underneath, a beanie, scarf and heavy boots",
    "a quilted mustard jacket, a grey knit hat with ear flaps, thick gloves and snow boots",
    "a long navy parka with a fur-trimmed hood, wool trousers and insulated boots",
    "a patchwork knit poncho over a turtleneck, leggings and fur boots",
    "a classic red ski sweater with a white pattern, ski trousers and mittens",
    "a heavy canvas coat with a corduroy collar, a thermal shirt, work trousers and boots",
    "a silver metallic puffer, a black balaclava pushed down around the neck, jeans and chunky boots"};
const std::vector<std::string> kWarm = {
    "a loose Hawaiian-print shirt, linen shorts, sunglasses and sandals",
    "a flowing sundress or breezy linen outfit in soft yellow, a straw hat and sandals",
    "a white tank top, light denim shorts, a bandana and sneakers",
    "a bright short-sleeved beach shirt over a tank top, patterned shorts, a towel over one shoulder and flip-flops",
    "a pastel polo, chino shorts, a baseball cap and white sneakers",
    "a crochet top, wide-leg cotton trousers, round sunglasses and a woven bag",
    "a sleeveless coral shirt, light cargo shorts, a sun hat and canvas shoes",
    "a tie-dye tee, relaxed shorts, a friendship bracelet stack and sandals",
    "a bright orange linen shirt left open over a white tee, khaki shorts, a straw hat and sandals",
    "a flowy printed maxi dress or wide kaftan, big sunglasses and a woven hat",
    "a sleeveless denim vest over a tee, cut-off shorts, a bandana and worn sneakers",
    "a short-sleeved resort shirt in a retro palm print, white trousers and loafers",
    "a bright yellow raincoat-free summer set: crop top, high-waist shorts, a bucket hat and platform sandals",
    "a loose cotton tunic, wide trousers, a headscarf against the sun and leather sandals"};
const std::vector<std::string> kNight = {
    "a glossy black leather jacket, a metallic silver top, dark trousers and boots, with glitter on the cheekbones",
    "a sharp velvet blazer in deep blue, a black shirt and polished shoes",
    "a sparkling sequin outfit with bold neon accessories",
    "an oversized neon-print bomber jacket, a mesh top, cargo pants and platform sneakers",
    "a sleek dark cocktail outfit with gold jewelry",
    "a vintage band tee, ripped jeans, a studded belt and combat boots",
    "a satin shirt in emerald green, black slacks and statement earrings",
    "a futuristic reflective jacket, wraparound visor glasses and tech-wear trousers",
    "a holographic silver jacket, a black mesh top, wide cargo trousers and glowing sneakers",
    "a sleek black turtleneck, a long coat, dark glasses and gloves, in a noir style",
    "a vintage 80s outfit: shoulder pads, bright patterns, leg warmers and big earrings",
    "a rhinestone-studded denim jacket over a black dress or shirt and boots",
    "a flowing velvet cape over an elegant dark outfit, with dramatic eyeliner",
    "a bright coloured suit with a loud shirt and platform shoes, ready for the dance floor"};
const std::vector<std::string> kFormal = {
    "a tailored charcoal suit with a crisp white shirt and a thin tie",
    "an elegant floor-length evening outfit in deep red with delicate jewelry",
    "a navy three-piece suit with a pocket square and polished brogues",
    "a cream blazer over a silk blouse or shirt, tailored trousers and loafers",
    "a classic black tuxedo with a bow tie, or a black formal gown",
    "a burgundy velvet dinner jacket, a black shirt and black dress shoes",
    "a smart pencil-skirt or tailored-trouser office outfit in dove grey with a structured bag",
    "a pale linen suit, an open collar and brown leather shoes",
    "a forest green velvet blazer, black trousers, a bow tie and polished oxfords",
    "an elegant silk dress or suit in midnight blue with a delicate necklace",
    "a pinstriped waistcoat over a white shirt with sleeve garters, a pocket watch and dress boots",
    "a sharp white tuxedo jacket, black bow tie and patent shoes",
    "a traditional formal robe or kimono-style outfit in deep colours with a sash",
    "a fitted burgundy blazer over a cream turtleneck, tailored trousers and brown brogues"};
const std::vector<std::string> kActive = {
    "a bright blue technical running jacket, black leggings or shorts and trail shoes",
    "a loose hoodie, jogger pants, a cap and chunky sneakers",
    "a rugged olive hiking jacket, cargo trousers, sturdy boots and a backpack",
    "a sleeveless training top, short shorts, wristbands and a sweatband",
    "a retro tracksuit with stripes down the sleeves and clean white trainers",
    "a flannel shirt tied at the waist over a tee, work trousers and hiking boots",
    "a cycling jersey, padded shorts, a helmet pushed back and sport sunglasses",
    "a fleece vest over a long-sleeved shirt, softshell trousers and trail runners",
    "a bright yellow cycling jacket, black bib shorts, sport sunglasses and a helmet",
    "a lightweight windbreaker, running shorts, a headband and bright running shoes",
    "a green waxed hiking jacket, convertible trousers, gaiters and a wide-brim hat",
    "a martial arts uniform with a coloured belt, bare feet and tape on the hands",
    "a team jersey with a number, shorts, long socks, wristbands and cleats",
    "a thick fleece pullover, thermal leggings, a trucker cap and mountaineering boots"};
const std::vector<std::string> kCozy = {
    "an oversized chunky knit cardigan, soft leggings and thick fuzzy socks",
    "a faded band hoodie, pyjama trousers and slippers",
    "a flannel shirt, comfortable jeans and house socks",
    "a soft pastel sweater with rolled sleeves, a long skirt or relaxed trousers and a messy bun",
    "a baggy university sweatshirt, joggers and a wool blanket over the shoulders",
    "a striped apron over a simple tee and jeans, sleeves rolled up",
    "a velvet robe over a tee, with reading glasses pushed into the hair",
    "a mustard turtleneck, high-waist trousers and warm slippers",
    "a thick cable-knit jumper dress or oversized sweater, patterned socks and a hair clip",
    "a soft hooded blanket-robe, slippers and a mug-warm sleepy look",
    "a cardigan with leather elbow patches, reading glasses on a chain, corduroys and house shoes",
    "a chunky wool vest over a long-sleeved tee, jeans and wool socks",
    "a floral apron, rolled-up sleeves, flour on the hands and sensible shoes",
    "a vintage dressing gown with a paisley print over pyjamas and worn slippers"};
const std::vector<std::string> kGeneric = {
    "a vivid red jacket over a white tee, black jeans and clean sneakers",
    "an emerald green knit sweater, beige chinos and brown boots",
    "a denim jacket over a striped shirt, dark jeans and canvas shoes",
    "a long camel coat, a black turtleneck, tailored trousers and boots",
    "a bright yellow raincoat-style jacket, a grey hoodie underneath, joggers and sneakers",
    "a bold patterned shirt, high-waisted trousers and round glasses",
    "a leather jacket, a graphic tee, ripped jeans and boots",
    "a soft lilac sweater, a pleated skirt or relaxed trousers and white sneakers",
    "a crisp white shirt with the sleeves rolled up, suspenders and a flat cap",
    "a retro 70s outfit: flared trousers, a printed shirt and tinted sunglasses",
    "a sporty windbreaker in teal and orange, black shorts or leggings and trainers",
    "a smart blazer over a plain tee, slim jeans and loafers",
    "a burnt orange hoodie, black cargo pants and chunky white sneakers",
    "a navy blazer, a white tee, light chinos and tan loafers",
    "a floral shirt, olive trousers and canvas sneakers",
    "a long grey cardigan over a striped tee, jeans and ankle boots",
    "a teal bomber jacket, a graphic hoodie, black jeans and high-top sneakers",
    "a plaid overshirt, a white tee, dark jeans and work boots",
    "a sleeveless puffer vest over a long-sleeved shirt, slim trousers and sneakers",
    "a pastel suit with a white tee and white trainers",
    "a rugged leather vest over a shirt with rolled sleeves, belts and boots",
    "a dusty rose coat, a cream scarf, jeans and white boots",
    "an oversized football-style jersey, baggy jeans and a cap worn backwards",
    "a silk bomber with an embroidered back, black tee, tapered trousers and slides"};
const std::vector<std::string> kFeminine = {
    "a flowing midi dress with a denim jacket and ankle boots", "a cropped cardigan over a camisole, a high-waisted skirt and sneakers",
    "a wrap dress in deep teal with simple jewelry and low heels"};
const std::vector<std::string> kMasculine = {
    "a fitted bomber jacket over a henley shirt, chinos and sneakers", "an unbuttoned patterned shirt over a plain tee, jeans and boots",
    "a knit polo, a light jacket, slim trousers and suede shoes"};

// a change that keeps the character's own clothes and style, adapted to the moment
const std::vector<std::string> kAdapt = {
    "the collar turned up and the jacket hanging open", "the sleeves pushed up to the elbows", "everything a little windswept and rumpled",
    "the hood pulled up", "the clothes slightly damp and clinging, with a few wet streaks", "a scarf or loose fabric blowing in the wind",
    "the top buttons undone and the clothes slightly loose, relaxed", "the clothes zipped up high to the chin", "the sleeves pulled down over the hands",
    "one sleeve slipping off a shoulder", "the cape or outer layer swirling around them", "a few smudges of dirt and dust, the clothes looking lived-in",
    "an accessory added that suits the moment (sunglasses, a hat or a bag)", "the outfit neat and sharply straightened, as if ready for the occasion",
    "a few tears and patches on the clothes, as if after a long journey",
    "the outer layer slung over one shoulder",
    "a belt or strap pulled tighter, ready for action",
    "the fabric catching the light and glowing at the edges",
    "frost or snow on the shoulders and hair",
    "petals, confetti or leaves caught in the clothes and hair",
    "everything dusty with sand and wind",
    "a splash of colour added to the outfit, like a scarf, ribbon or badge"};

const std::vector<std::string> kPose = {
    "weight on one leg, shoulders relaxed, head tilted toward the other person", "leaning in close with the upper body angled toward the others",
    "standing tall with the chin lifted and one hand on the hip", "mid-step, walking, looking back over one shoulder",
    "slouched casually with both hands in the pockets", "bending slightly forward at the waist, hands on the knees",
    "arms crossed, leaning back against whatever is behind", "one arm raised high, the other hand pointing at something off-camera",
    "turned three-quarters away, glancing back at the camera", "crouching slightly, ready to move, hands open",
    "leaning on one elbow with the body twisted and relaxed", "stretching both arms wide, chest open",
    "shoulders hunched and head pulled in, the whole body tense", "hands clasped behind the back, rocking on the heels",
    "one hand covering the mouth, the body bent forward", "hands cupped around the mouth as if calling out, leaning back",
    "spinning or twirling, the clothes and hair swinging", "walking forward fast with a confident stride and swinging arms",
    "perched on the edge of something, legs crossed, leaning on a hand", "arms around the shoulders of whoever is closest",
    "sitting on the ground with the knees up and arms wrapped around them",
    "mid-jump, both feet off the ground",
    "leaning forward with both hands on a table or railing",
    "back turned, looking over the shoulder with the head tilted",
    "walking backwards while pointing at someone",
    "kneeling on one knee, one hand on the ground",
    "sitting sideways on a ledge with the legs dangling",
    "standing on tiptoes, stretching up to reach something",
    "arms thrown up in triumph",
    "hand shading the eyes while looking into the distance",
    "hugging themselves, shoulders raised against the cold",
    "head resting on a hand, elbow on the knee, thinking",
    "mid-stride with a heavy bag, leaning into the wind",
    "one foot on a step, leaning on the raised knee",
    "clapping with both hands, bouncing on the spot",
    "crossing the arms with a stubborn tilt of the head",
    "lying propped on one elbow, relaxed",
    "balancing on a low wall with the arms out",
    "ducking low with one arm raised to protect the head",
    "twisting around mid-run to look back"};
const std::vector<std::string> kExpression = {
    "laughing out loud with the eyes squeezed shut", "wide-eyed surprise with the mouth open", "a cheeky smirk with one eyebrow raised",
    "a warm, gentle smile looking at someone", "a focused, determined frown", "a shocked gasp with both eyebrows high",
    "a goofy grin showing the teeth", "a dreamy, faraway look", "a fierce scowl with clenched teeth", "tearful eyes and a trembling, happy smile",
    "an exaggerated silly face with the tongue out", "calm, confident and unbothered, a slight smile", "a worried frown looking sideways",
    "bursting into delighted laughter, head thrown back", "a suspicious squint, the lips pressed together", "an astonished open-mouthed stare",
    "a sleepy yawn", "a proud beaming smile with the chin up", "an embarrassed blush with a shy half-smile", "a mischievous grin as if planning something",
    "a wide, relieved grin with the eyes closed",
    "a stunned, speechless stare",
    "a smug half-smile with the chin raised",
    "a brave, tight-lipped determination",
    "a sheepish smile with a hand behind the head",
    "raised eyebrows and a flat 'really?' look",
    "a delighted gasp with both hands near the face",
    "an exhausted but happy smile",
    "a theatrical gasp of outrage",
    "a wicked, playful grin",
    "a soft, tearful smile of pure affection",
    "a squint into the wind with a determined jaw",
    "a fierce battle-cry with a wide-open mouth",
    "an anxious, wide-eyed glance at something off-camera",
    "a serene closed-eye smile as if listening to music",
    "a mock-serious frown trying not to laugh"};

const std::vector<std::string> kPlace = {
    "a rainy neon-lit city street at night", "a sunlit flower market", "a flooded library with books floating", "a rooftop at sunset above a busy city",
    "a quiet snowy village square", "a crowded night market with lanterns", "a train carriage moving through the countryside", "a tiny bookshop café on a rainy afternoon",
    "an abandoned amusement park overgrown with plants", "a foggy forest path", "a busy railway station platform", "a retro diner with chrome stools and jukebox",
    "a sandy beach at golden hour", "a mountain cabin interior with a crackling fireplace", "an old stone bridge over a river at dawn", "a neon arcade full of glowing machines",
    "a grand ballroom with chandeliers", "a messy artist's studio with canvases everywhere", "a wide desert road with a parked old car", "a laundromat late at night",
    "a bustling street food stall in the rain", "a school rooftop in the afternoon wind", "a vast field of sunflowers", "a cosy kitchen with something burning on the stove",
    "a space station corridor with a window onto Earth", "a cobblestone alley with hanging laundry", "a skate park at dusk", "a lighthouse on a stormy cliff",
    "an underground metro station with flickering lights", "a lakeside jetty at sunrise", "a fairground with a giant ferris wheel", "a medieval market square",
    "a futuristic city plaza with floating signs", "a gym with chalk dust in the air", "a Japanese shrine path with red gates", "a cluttered attic full of old trunks",
    "a rooftop pool party", "a mossy ruined temple in the jungle", "a snowy mountain pass with prayer flags", "an old cinema lobby with a popcorn machine",
    "a harbour with fishing boats at dawn", "a hospital waiting room at night, oddly funny", "a hedge maze in autumn", "a vineyard terrace at harvest time",
    "a campsite with a bonfire under the stars", "a food court in a giant mall", "a bamboo forest after rain", "a wedding reception in a garden",
    "a cyberpunk noodle bar", "a roller disco", "a farmhouse porch in a thunderstorm", "a ski lodge bar", "a seaside promenade with ice cream stands",
    "a courtyard of a crumbling castle", "a rainy bus stop", "a studio set during a film shoot", "a quiet temple garden in spring", "a theatre stage with the curtains half open",
    "a snow-covered pine forest at night", "a busy airport gate", "a haunted-looking mansion hallway", "a go-kart track", "an orchard in bloom", "a cliffside road trip viewpoint",
    "a rooftop garden above a quiet neighbourhood",
    "a glass greenhouse full of giant tropical plants",
    "a cable car cabin high above a valley",
    "a rainy night-time gas station",
    "a floating market of boats in the early morning",
    "an old observatory with a open dome and a huge telescope",
    "a bridge between two skyscrapers at dusk",
    "a gondola in a misty canal",
    "the back alley behind a ramen shop",
    "a derelict swimming pool covered in graffiti",
    "a hot-air balloon basket above the clouds",
    "a snowy cabin porch with a view of the aurora",
    "a street festival with giant paper dragons",
    "a cosy tavern with a roaring fireplace",
    "an arctic research station in a blizzard",
    "a bustling night bazaar with spice stalls",
    "a flooded city street with shops and boats",
    "a trampoline park after closing time",
    "a tiny rooftop observatory under a meteor shower",
    "a mountain hot spring in the snow",
    "a vintage record shop with stacked vinyl",
    "a pillow-fort living room in the middle of the day",
    "an overgrown greenhouse inside a ruined palace",
    "a ferry deck in a storm",
    "a bamboo raft drifting down a jungle river",
    "a chess park with giant pieces",
    "a rooftop drive-in cinema at night",
    "a cherry-blossom path at the peak of spring",
    "a crowded subway car at rush hour",
    "an empty school corridor with golden light",
    "a lantern-lit stone staircase in an old town",
    "a steaming street of a city in winter",
    "a glowing coral reef seen through a glass tunnel",
    "a lava-lit cave with glittering crystals",
    "a haunted-looking carnival tent",
    "a desert caravan camp at sunset",
    "a spaceship cockpit with blinking lights",
    "a pirate ship deck in rough seas",
    "a medieval library with candlelit tables",
    "a witch's cottage kitchen full of herbs and jars",
    "an enchanted forest clearing with floating lights",
    "a giant clock tower interior with huge gears",
    "a wizard's study with floating books",
    "a dragon's treasure cave",
    "a cloud city with rope bridges",
    "a steampunk airship lounge",
    "a post-apocalyptic overgrown highway",
    "a neon-soaked rainy rooftop in a cyberpunk city",
    "a robot repair shop with sparks flying",
    "a floating island with waterfalls",
    "an ancient arena with cheering crowds",
    "a samurai castle courtyard in the snow",
    "a vineyard cellar with huge oak barrels",
    "a snow-covered train platform in a tiny countryside station",
    "a lively beach volleyball court",
    "a cluttered inventor's workshop",
    "a ruined cathedral with sunbeams",
    "a bakery at 4 a.m. with flour in the air",
    "a roller coaster car at the top of the first drop",
    "a hot desert diner with a neon sign",
    "an ice-skating rink with fairy lights",
    "a bus stop in the middle of nowhere at dusk",
    "a quiet lakeside pier in fog",
    "a bustling harbour fish market",
    "a library reading room under a glass dome",
    "a skatepark bowl at golden hour",
    "a backstage dressing room full of mirrors and lights",
    "a lighthouse keeper's spiral staircase",
    "a night-time rooftop with laundry lines and string lights",
    "a country road with a broken-down van",
    "a sunken ship's ballroom",
    "a glass elevator rising through a huge atrium",
    "a toy-filled attic with a rocking horse",
    "a rice-terrace hillside in morning mist",
    "a market street in a snowy mountain town",
    "a retro space-age diner on the moon",
    "an underground jazz club with smoke and blue light",
    "a haunted-house hallway with floating candles",
    "a giant treehouse village in the canopy",
    "a stormy cliff-top castle ruin",
    "a flower-filled courtyard of a Moroccan riad",
    "a busy kitchen of a restaurant at full tilt",
    "a tiny islet with one palm tree",
    "a hillside village with painted houses",
    "a toy store aisle at night, the toys half alive",
    "a giant mushroom forest glowing faintly",
    "a fish-eye view of a crowded plaza during a festival"};
const std::vector<std::string> kTime = {
    "at golden hour", "at blue hour just after sunset", "at midday under a harsh sun", "at dawn with soft mist", "deep at night with warm street lights",
    "in heavy rain", "in light snowfall", "on a windy overcast afternoon", "in thick fog", "at sunset with long shadows", "at night under a full moon",
    "during a thunderstorm with flashes of lightning", "on a crisp autumn morning with falling leaves", "on a hot summer afternoon", "in the middle of a power outage with candlelight",
    "just before sunrise with a pink-orange sky",
    "in the middle of a heatwave, the air shimmering",
    "during a golden autumn afternoon",
    "under a sky full of stars and the Milky Way",
    "on a snowy evening with soft falling flakes",
    "during a rainstorm with the streets reflecting every light",
    "in the blue silence before a thunderstorm",
    "with an eclipse turning the light strange",
    "at a bright spring noon with petals in the wind",
    "on a foggy dawn with the sun just a glow",
    "in a sudden hailstorm",
    "late at night with only neon and street lights"};
const std::vector<std::string> kMood = {
    "a warm, cozy indie film still", "a chaotic slapstick comedy moment", "an epic blockbuster action movie still", "a tender romantic drama",
    "a spooky but funny horror-comedy", "a nostalgic 90s photograph", "a high-energy music video frame", "a quiet, melancholic arthouse shot",
    "an adventurous fantasy poster", "a glossy fashion magazine editorial", "a gritty noir crime scene", "a joyful holiday card",
    "a surreal dream with odd details", "a cinematic sci-fi thriller", "an awkward-but-sweet sitcom scene", "a candid paparazzi snapshot",
    "an anime-style key visual with cinematic lighting", "a heroic last-stand moment", "a wholesome slice-of-life moment", "a dramatic cliffhanger",
    "a cozy winter holiday movie scene",
    "a dramatic fantasy epic frame",
    "a tense heist-movie moment",
    "a goofy buddy-comedy poster",
    "a sweeping romantic movie finale",
    "an eerie but beautiful supernatural scene",
    "a retro 80s action poster",
    "a bright, playful children's book illustration made photographic",
    "a moody cyberpunk night scene",
    "a heartwarming reunion",
    "a chaotic food-fight comedy",
    "a stylish spy-movie scene",
    "a wistful coming-of-age film frame",
    "a high-fashion campaign shot",
    "a wild adventure documentary still",
    "a cinematic western standoff"};
const std::vector<std::string> kTwist = {
    "confetti is falling everywhere", "a street musician is playing right behind them", "a big dog is photobombing in the foreground",
    "everything is lit by colourful fairy lights", "a sudden gust of wind sends papers and leaves flying", "a paper lantern floats past",
    "someone's hat has just blown off", "a pigeon has landed on the nearest shoulder", "balloons drift across the frame", "sparks and steam burst out of a nearby machine",
    "a small crowd is watching and clapping in the background", "snowflakes and petals mix in the air", "a spilled drink splashes in mid-air", "a flock of birds takes off behind them",
    "a rainbow appears after the rain", "an old radio on the ground is playing music", "bubbles float all around", "a cat sits calmly watching the scene",
    "a mirror reflects the scene from another angle", "smoke from a food stall drifts through the light",
    "a small dog runs through the frame with a stolen sock",
    "a street performer juggles fire in the background",
    "lanterns rise slowly into the sky",
    "a sudden gust of wind turns an umbrella inside out",
    "a vendor's cart is rolling away by itself",
    "a flash from a camera lights everything white for an instant",
    "a bird has landed on someone's head",
    "a stack of boxes tumbles over in the background",
    "a huge moon fills the sky",
    "fireworks light the sky behind them",
    "a toy robot is marching past the feet",
    "an old gramophone is playing and nobody knows why",
    "a trail of paint footprints leads across the ground",
    "a kite is tangled in a nearby tree",
    "someone nearby is sneezing dramatically",
    "a cat has stolen a sandwich",
    "a street lamp flickers and goes out",
    "a rain of petals falls from above",
    "a crowd is staring up at something outside the frame",
    "a giant balloon animal looms behind them",
    "a postman is delivering a very large parcel",
    "steam rises from a grate at their feet",
    "a tiny dragon-shaped kite swoops past",
    "lightning splits the sky in the distance",
    "a spilled bag of oranges is rolling everywhere",
    "a streetlight casts a perfect spotlight on them",
    "a double rainbow arches overhead",
    "paper planes are flying everywhere",
    "a reflection in a puddle shows something different",
    "a fog bank rolls in around their ankles"};
const std::vector<std::string> kRelation = {
    "takes a selfie with", "dances with", "argues with", "laughs together with", "hugs", "races", "cooks a meal with", "sneaks up on", "chases", "stands back to back with",
    "high-fives", "whispers a secret to", "shares an umbrella with", "teaches a trick to", "carries on their back", "shows a map to", "toasts with", "plays a game with",
    "explores with",
    "hides from",
    "rescues",
    "follows",
    "sneaks past",
    "escapes with",
    "searches for clues with",
    "carries to safety",
    "shares an umbrella with",
    "teaches a skill to",
    "plays a board game with",
    "walks with",
    "waits impatiently with",
    "builds something with",
    "does a trick for",
    "faces off with",
    "bargains with",
    "forgives",
    "stares down",
    "swaps hats with",
    "dares",
    "mimics",
    "scares",
    "trips over",
    "steals the snack of",
    "wakes up"};

const std::vector<std::string> kProp = {
    "an old brass lantern", "a giant umbrella", "a paper map spread out", "a vintage camera", "a glowing crystal", "a steaming mug", "a stack of old books", "a big red balloon",
    "a skateboard", "an acoustic guitar", "a small suitcase", "a sparkler", "a polaroid photo", "a basket of fruit", "a pair of binoculars", "a toy sword", "a bunch of keys on a ring",
    "a boombox", "a telescope", "a bouquet of wildflowers", "a bag of popcorn", "a typewriter", "a bicycle", "a mysterious sealed envelope", "a pizza box", "a watering can",
    "a lit candle", "a megaphone", "a snow globe", "a sketchbook", "a baguette", "a giant lollipop", "a pocket watch", "a rubber duck", "a treasure chest", "a flashlight",
    "a cup of bubble tea", "a pair of roller skates", "a kite", "a fishing rod", "a magic wand", "a rolled-up carpet", "a cardboard sign", "a tiny robot", "a trumpet",
    "an ice cream cone", "a garden gnome", "a lantern on a pole", "a notebook full of scribbles", "a bucket of paint", "a pair of oversized sunglasses", "a hand fan",
    "a trophy", "a wheelbarrow", "a rope", "a rain-soaked newspaper", "a tray of cupcakes", "a camping stove", "a very large wrench", "a violin"};
const std::vector<std::string> kPhotoStyle = {
    "shot on 35mm film with visible grain and warm colours", "a crisp modern full-frame photograph with shallow depth of field", "a faded 90s disposable-camera snapshot with a flash",
    "a cinematic anamorphic frame with lens flares and teal and orange grading", "a soft dreamy photograph with a glowing haze", "a high-contrast black-and-white film photograph",
    "a vivid medium-format fashion photograph", "an overexposed summer holiday photo with bright highlights", "a moody low-key photograph with deep shadows and a single light source",
    "a retro Polaroid with washed colours", "a punchy pop-art colour grade with strong saturation", "a documentary-style candid photograph with natural light",
    "a golden-hour backlit photograph with long shadows", "a wide-angle action-camera feel with slight distortion", "a miniature tilt-shift look",
    "a neon-lit night photograph with colourful reflections", "a painterly, soft-focus photograph like an old oil painting", "a clean studio-flash editorial photograph",
    "a grainy night photograph with high ISO noise and street lights", "a film-still look with a slight vignette and halation"};
const std::vector<std::string> kBeat = {
    "they have only just arrived and are looking around", "a surprise is discovered", "an argument is brewing", "a chase begins", "they are in the middle of the chaos",
    "it all goes wrong in a funny way", "a quiet moment of rest after the trouble", "they celebrate together", "someone has an idea", "a plan is being made",
    "an unexpected visitor appears", "they are lost and trying to find the way", "the tension is at its highest", "they make up after an argument",
    "they say goodbye", "they stumble into something strange", "the big moment has arrived", "they are exhausted but happy"};

// ---- eras: when (and in what world) a scene happens. Each has its own places and outfits, so a medieval scene never ends up in a laundromat.
struct Era {
    const char* name;   // "medieval"
    const char* phrase; // "in the Middle Ages"
    bool realWorld;     // countries and cities make sense with it
    std::vector<std::string> places, outfits;
};

const std::vector<Era>& Eras() {
    static const std::vector<Era> eras = {
        {"present day", "", true, {}, {}}, // uses the big modern lists
        {"medieval", "in the Middle Ages", false,
         {"a castle courtyard with banners in the wind", "a busy medieval market square", "a candlelit tavern with a roaring hearth", "a stone bridge over a misty river", "a monastery cloister with tall arches",
          "a village green during a harvest fair", "a castle's great hall with long tables", "a muddy road between half-timbered houses", "a watchtower on a windy hill", "a knight's tournament field with colourful tents"},
         {"a wool tunic with a leather belt, hose and sturdy boots", "a long hooded cloak over a linen shirt and leather jerkin", "a rich velvet surcoat with embroidered trim and a jeweled belt",
          "a chain-mail shirt under a plain tabard, gloves and boots", "a long layered gown with hanging sleeves and a veil, or a layered tunic with a hood", "a monk-style rough robe with a rope belt",
          "a merchant's fur-trimmed coat with a coin purse and a feathered cap", "a leather hunting outfit with a quiver and a green hood"}},
        {"ancient world", "in the ancient world", false,
         {"a marble forum full of traders and orators", "a sun-baked temple courtyard with tall columns", "a colosseum arena packed with a roaring crowd", "a Nile riverbank with reed boats and palm trees",
          "a bustling harbour of an ancient trading city", "a torch-lit pyramid corridor with painted walls", "a hilltop acropolis at sunrise", "a Roman bathhouse full of steam"},
         {"a white draped toga with a gold clasp and leather sandals", "a pleated linen kilt, a broad gold collar and heavy eyeliner", "a bronze breastplate and crested helmet over a red tunic",
          "a flowing chiton belted at the waist with a laurel wreath", "a robe of deep purple silk with golden jewelry", "a simple linen tunic with a woven belt and a headcloth"}},
        {"feudal Japan", "in feudal Japan", false,
         {"a cherry-blossom path to a mountain shrine", "a castle courtyard in the snow", "a bamboo forest in morning mist", "a lantern-lit post-town street at night", "a tea house garden with a stone lantern",
          "a wooden bridge over a misty gorge", "a rice paddy village at dawn", "a dojo with sunlight through paper screens"},
         {"a dark kimono with a wide sash and a sword at the hip", "a samurai's layered armor with a horned helmet under one arm", "a travelling cloak, a straw hat and sandals",
          "an elegant silk kimono with a floral pattern and hair ornaments", "a hakama and a plain jacket with rolled sleeves", "a ninja's dark wrapped outfit with a hood"}},
        {"Renaissance", "in the Renaissance", true,
         {"a painter's studio with canvases and a skylight", "a candlelit palazzo ballroom", "a canal-side street in a merchant city", "a cathedral piazza full of market stalls", "a library with globes and maps",
          "an inventor's workshop with strange machines", "a vineyard terrace overlooking rolling hills"},
         {"a rich brocade doublet with puffed sleeves, hose and a feathered hat", "a velvet gown with a tight bodice, a pearl necklace and an elegant hairnet",
          "a black scholar's robe with a flat cap and a book under the arm", "an artist's smock stained with paint over a linen shirt", "a merchant's fur-lined coat with a heavy gold chain"}},
        {"Victorian", "in the Victorian era", true,
         {"a gas-lit cobbled London-style street in fog", "a steaming railway station with iron arches", "a grand drawing room with heavy curtains", "a smoky factory district at dusk",
          "a foggy riverside dock with tall ships", "a conservatory full of exotic plants", "a detective's cluttered study with maps and a pipe"},
         {"a tailored tweed suit with a waistcoat, a pocket watch and a bowler hat", "a bustled dress with a high lace collar, gloves and a parasol", "a long dark overcoat, a top hat and a walking cane",
          "a working-class flat cap, braces and rolled sleeves", "an explorer's pith helmet, riding jacket and boots", "a nurse or governess's plain dark dress with a white apron"}},
        {"Wild West", "in the Wild West", false,
         {"a dusty frontier main street with a saloon", "a canyon trail at sunset", "a railway halt in the desert", "a saloon with swinging doors and a piano", "a ranch porch under a huge sky",
          "a ghost town with tumbleweeds", "a river crossing with a covered wagon"},
         {"a wide-brim hat, a leather vest, a bandana and dusty boots with spurs", "a long duster coat, a gun belt and a battered hat", "a calico dress with an apron and a sunbonnet",
          "a sheriff's star, a waistcoat and a neat hat", "a poncho, a sombrero-style hat and worn boots", "a saloon dancer's fringed dress with feathers"}},
        {"Roaring Twenties", "in the 1920s", true,
         {"a smoky jazz club with a brass band", "a speakeasy behind a barber shop", "a grand art deco hotel lobby", "a vintage motorcar on a wet city street", "a rooftop party above a glittering city",
          "a train dining car with white tablecloths"},
         {"a pinstripe suit with a fedora and spats", "a beaded flapper dress, a feathered headband and long pearls", "a three-piece suit with a straw boater",
          "a drop-waist dress with a cloche hat and T-bar shoes", "a tuxedo with a white scarf and a cigarette holder"}},
        {"1950s", "in the 1950s", true,
         {"a chrome diner with a jukebox", "a drive-in cinema at dusk", "a neat suburban street with big cars", "a bowling alley with neon lights", "a soda fountain counter", "a roadside motel with a neon sign"},
         {"a leather jacket, rolled jeans and a white tee with slicked hair", "a polka-dot swing dress with a petticoat and a headscarf", "a letterman jacket with a pleated skirt or chinos and saddle shoes",
          "a grey flannel suit with a narrow tie and a hat", "a poodle skirt with a cardigan and bobby socks"}},
        {"1980s", "in the 1980s", true,
         {"a neon-lit video arcade", "a roller rink with a mirror ball", "a shopping mall food court with fountains", "a suburban garage full of bikes and cables", "a cassette-lined bedroom with posters",
          "a rainy city street with neon signs"},
         {"an oversized pastel blazer with shoulder pads and a bright tee", "a denim jacket covered in badges, ripped jeans and high-tops", "a neon windbreaker, a headband and legwarmers",
          "a leather jacket with the sleeves pushed up and a skinny tie", "a big colourful knit sweater and acid-wash jeans"}},
        {"1990s", "in the 1990s", true,
         {"a video rental shop with shelves of tapes", "a high-school hallway with lockers", "a sleepy suburban cul-de-sac", "a cyber cafe with chunky computers", "a skate plaza with graffiti", "a mall's escalator atrium"},
         {"baggy jeans, an oversized flannel shirt and chunky sneakers", "a baby tee, wide cargo trousers and platform sandals", "a windbreaker with bold blocks of colour, bucket hat and a backpack",
          "a slip dress over a plain tee with combat boots", "a tracksuit with a puffer vest and a small backpack"}},
        {"near future", "in the near future", true,
         {"a neon-lit rain-soaked megacity street", "a vertical farm tower full of greenery", "a magnetic train platform in a glass station", "a rooftop with drones buzzing overhead", "a holographic night market",
          "a cramped repair stall full of cables and screens"},
         {"a glossy reflective jacket with glowing trim, tech-wear trousers and visor glasses", "a minimalist grey bodysuit with a long transparent coat", "a patched jacket full of LEDs, a respirator mask and boots",
          "a clean white utility jumpsuit with a badge and a small drone on the shoulder", "a neon-edged cape over layered black clothes"}},
        {"far future", "in the far future", false,
         {"a space station corridor with a window onto a ringed planet", "a starship bridge with glowing consoles", "a domed colony city on an alien world", "a dusty mining outpost on a moon", "an orbital elevator terminal",
          "an alien bazaar full of strange creatures", "a bioluminescent alien jungle"},
         {"a sleek flight suit with patches and a helmet under the arm", "a long robe of shimmering fabric with a glowing collar", "a patched explorer's jumpsuit with tool belts and goggles",
          "a captain's uniform with gold trim and a high collar", "light armor with a visor and a holstered blaster", "an elegant silver tunic with a floating cape"}},
        {"post-apocalyptic", "in a post-apocalyptic wasteland", false,
         {"an overgrown highway full of rusted cars", "a ruined city with vines climbing the towers", "a scrapyard settlement made of containers", "a dried-up reservoir with a lonely shack",
          "a flooded subway station lit by fires", "a bunker corridor with flickering lights"},
         {"a patched leather jacket, a scarf over the face, goggles and worn boots", "a mismatched armor of scrap metal and rubber over ragged clothes", "a long dusty coat with many pockets, a satchel and a gas mask on the chest",
          "a hand-stitched poncho, wrapped boots and a homemade bow", "layers of faded work clothes with a tool belt and a cap"}},
        {"high fantasy world", "in a high fantasy world", false,
         {"an elven treehouse city with glowing lanterns", "a dragon's mountain lair full of gold", "a wizard's tower library with floating books", "a dwarven forge hall with rivers of lava",
          "an enchanted forest clearing with giant mushrooms", "a floating island with waterfalls", "a haunted swamp village on stilts", "a crystal cave with glowing runes"},
         {"an embroidered mage's robe with a pointed hat and a staff", "gleaming plate armor with a crest and a long cape", "a ranger's green cloak, leather armor and a longbow", "an ornate gown of silver silk with a circlet",
          "a rogue's dark hooded leathers with daggers and pouches", "a bard's colourful patchwork coat with a lute"}},
        {"pirate golden age", "in the age of pirates", false,
         {"a pirate ship's deck in a storm", "a hidden cove with a treasure chest", "a tropical harbour tavern full of sailors", "a jungle trail on a remote island", "a captain's cabin with maps and candles",
          "a stormy rope-bridge between two sea cliffs"},
         {"a long red coat with brass buttons, a tricorn hat and a cutlass", "a loose white shirt, a striped sash, a bandana and tall boots", "a captain's navy coat with gold braid and a feathered hat",
          "a patched sailor's jacket, rolled trousers and bare feet with an earring", "a leather corset-vest over a billowing shirt with a scarf and a pistol belt"}},
        {"film-noir 1940s", "in a 1940s noir city", true,
         {"a rainy city street with a flickering neon sign", "a smoky detective office with venetian blinds", "a dim jazz bar with a lone saxophone", "a foggy dock with crates and lamp posts", "a rooftop with a water tower at night",
          "a train station platform wrapped in steam"},
         {"a trench coat with the collar up and a fedora pulled low", "a tailored dark suit with a loosened tie and a hat", "a long satin dress with gloves and a veiled hat",
          "a rumpled shirt, suspenders and a shoulder holster", "a wide-shouldered coat and a clutch purse with a polished hat"}},
    };
    return eras;
}

const std::vector<std::string> kRegion = {
    "Tokyo, Japan", "Kyoto, Japan", "Osaka, Japan", "Seoul, South Korea", "Beijing, China", "Shanghai, China", "Hong Kong", "Singapore", "Bangkok, Thailand", "Hanoi, Vietnam", "Mumbai, India",
    "Delhi, India", "Jaipur, India", "Kathmandu, Nepal", "Dubai, UAE", "Istanbul, Turkey", "Cairo, Egypt", "Marrakech, Morocco", "Cape Town, South Africa", "Lagos, Nigeria", "Nairobi, Kenya",
    "Paris, France", "London, England", "Edinburgh, Scotland", "Dublin, Ireland", "Amsterdam, the Netherlands", "Berlin, Germany", "Prague, Czechia", "Vienna, Austria", "Venice, Italy", "Rome, Italy",
    "Florence, Italy", "Barcelona, Spain", "Lisbon, Portugal", "Athens, Greece", "Santorini, Greece", "Moscow, Russia", "Saint Petersburg, Russia", "Stockholm, Sweden", "Reykjavik, Iceland",
    "Copenhagen, Denmark", "Oslo, Norway", "Budapest, Hungary", "Krakow, Poland", "New York City, USA", "Los Angeles, USA", "San Francisco, USA", "Chicago, USA", "New Orleans, USA", "Las Vegas, USA",
    "Miami, USA", "Seattle, USA", "Austin, USA", "Honolulu, Hawaii", "Vancouver, Canada", "Toronto, Canada", "Montreal, Canada", "Mexico City, Mexico", "Havana, Cuba", "Rio de Janeiro, Brazil",
    "Buenos Aires, Argentina", "Lima, Peru", "Machu Picchu, Peru", "Cartagena, Colombia", "Santiago, Chile", "Sydney, Australia", "Melbourne, Australia", "Auckland, New Zealand", "Queenstown, New Zealand",
    "rural Japan", "the Scottish Highlands", "the Swiss Alps", "the Sahara desert", "the Amazon rainforest", "the Norwegian fjords", "Patagonia", "the Greek islands", "the Tuscan countryside",
    "the Himalayas", "the Icelandic highlands", "the Australian outback", "the Canadian Rockies", "the Mongolian steppe", "the Scandinavian arctic", "the American Midwest", "the Louisiana bayou",
    "a small town in Ireland", "a fishing village in Portugal", "a mountain village in Nepal", "a tiny island in the Caribbean", "a coastal town in Italy", "a countryside village in France",
    "a hill town in Spain", "a desert town in Morocco", "a night-time district of Tokyo", "a backstreet in Istanbul"};

const std::vector<std::string> kGenre = {
    "science fiction", "high fantasy", "film noir", "a western", "gothic horror", "a romantic comedy", "cyberpunk", "steampunk", "a wuxia martial-arts epic", "a spy thriller", "a heist caper",
    "a disaster movie", "a musical", "a superhero adventure", "a slice-of-life drama", "a mystery", "a space opera", "a pirate adventure", "a samurai drama", "a survival story", "a sports underdog story",
    "a courtroom drama", "a road movie", "a coming-of-age story", "a myth or legend", "a fairy tale", "an anime-style adventure", "a ghost story", "a time-travel story", "a monster-hunting story",
    "a political thriller", "a holiday family comedy", "a buddy-cop comedy", "a post-apocalyptic tale", "a detective story", "a war-time drama", "a cooking competition story", "a high-school drama",
    "an expedition into the unknown", "a mockumentary", "a rock-and-roll tour story", "a treasure hunt", "a haunted-house story", "a first-contact story", "a family saga", "a fantasy heist"};

const std::vector<std::string> kPlaceMore = {
    "an airport terminal at 3 a.m.", "the top of a skyscraper under construction", "a cosy second-hand bookshop with a sleeping cat", "a gym with a boxing ring", "a tattoo parlour full of flash art",
    "a night train sleeper cabin", "a hospital rooftop garden", "a newsroom full of ringing phones", "a lab full of bubbling glassware", "a city park chess table in autumn", "a barbershop with a striped pole",
    "a petrol station on a deserted highway", "a tiny cafe with steamed-up windows", "a skyscraper window-cleaner's platform", "a rooftop water tower", "a funeral home garden on a sunny day",
    "a busy university lecture hall", "a record-breaking dance marathon hall", "a ferry terminal in the rain", "a marina full of white yachts", "a campsite by a mountain lake", "a hot-air balloon festival field",
    "a famous street-art alley", "a high-street arcade of tiny shops", "an old tram crossing a square", "a lakeside boathouse at sunset", "a chilly bus station waiting room", "a city fountain at midnight",
    "a roof terrace restaurant above rooftops", "an art museum gallery with giant paintings", "a natural history museum with dinosaur skeletons", "a science museum with a giant pendulum", "a planetarium dome",
    "a vintage cinema's projection booth", "a fashion show backstage", "a wedding chapel in a desert town", "a ski lift in a snowstorm", "a rock-climbing wall with a view", "a surf shack on a windy beach",
    "an ice-fishing hut on a frozen lake", "a lavender field in bloom", "a tea plantation in morning mist", "a salt flat that mirrors the sky", "a hidden waterfall pool", "a cliffside monastery",
    "a bamboo bridge over a river gorge", "a floating lantern festival on a river", "a night-time rice field with fireflies", "a rope-swing over a swimming hole", "a fishing pier at dawn",
    "a crowded flea market", "a rooftop greenhouse cafe", "a candle-lit wine cellar", "a street-food alley with steam and neon", "a bustling covered market hall", "a tram stop in a hillside city",
    "a cable-car station above clouds", "a sleepy village post office", "a village well in a heatwave", "a grand staircase in an old opera house", "a lighthouse gallery in a gale", "a locksmith's cluttered shop",
    "a clockmaker's workshop with ticking walls", "a violin maker's workshop", "a cheese cave with rows of wheels", "a mill by a waterfall", "a shipyard with a half-built boat", "a train graveyard overgrown with moss",
    "a rain-soaked football pitch under floodlights", "a boxing gym at dawn", "a stage door in an alley", "a taxi stand in the rain", "a motorway service station at night", "a crowded stadium concourse",
    "a quiet monastery library", "a garden maze in summer", "a stone circle at dawn", "a canal lock with a waiting barge", "a drive-through at midnight", "a pier funfair with a ferris wheel"};

template <class T>
const T& PickOne(const std::vector<T>& v, std::mt19937& rng) {
    return v[rng() % v.size()];
}

// five distinct random candidate indices (or all of them when there are fewer)
std::vector<int> Candidates(size_t n, size_t k, std::mt19937& rng) {
    std::vector<int> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    std::shuffle(idx.begin(), idx.end(), rng);
    idx.resize(std::min(n, k));
    return idx;
}

int SampleFrom(const std::vector<double>& p, double temperature, std::mt19937& rng) {
    std::vector<double> w(p.size());
    double sum = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        w[i] = std::pow(std::max(1e-9, p[i]), 1.0 / std::max(0.2, temperature));
        sum += w[i];
    }
    double r = std::uniform_real_distribution<double>(0.0, sum)(rng);
    for (size_t i = 0; i < w.size(); ++i) {
        r -= w[i];
        if (r <= 0) return static_cast<int>(i);
    }
    return static_cast<int>(w.size()) - 1;
}

struct Q {
    std::string id, instructions;
    const std::vector<std::string>* options;
};

// A choice for every question: five random candidates each, and Clef (when it is running) says which fit; otherwise a random candidate.
std::vector<int> ChooseAll(const json& state, const std::vector<Q>& qs, bool useClef, std::mt19937& rng, bool& clefUsed) {
    std::vector<std::vector<int>> cand;
    std::vector<ClefQuestion> cq;
    for (const Q& q : qs) {
        cand.push_back(Candidates(q.options->size(), 5, rng));
        ClefQuestion c;
        c.id = q.id;
        c.instructions = q.instructions;
        for (int i : cand.back()) c.options.push_back((*q.options)[i]);
        cq.push_back(std::move(c));
    }
    std::vector<std::vector<double>> probs;
    clefUsed = useClef && !cq.empty() && ClefDecide(state.dump(), cq, probs, nullptr) && probs.size() == cq.size();
    const double temperature = ClefGetSettings().temperature;
    std::vector<int> out;
    for (size_t i = 0; i < qs.size(); ++i) {
        const int pick = clefUsed ? SampleFrom(probs[i], temperature, rng) : static_cast<int>(rng() % cand[i].size());
        out.push_back(cand[i][pick]);
    }
    return out;
}

enum class Gender { Unknown, Feminine, Masculine };

Gender GuessGender(const std::string& description) {
    const std::string d = " " + Lower(description) + " ";
    if (Has(d, {" woman", " girl", " female", " lady", " she ", " her ", " witch", " queen", " princess", " mother", " heroine"})) return Gender::Feminine;
    if (Has(d, {" man ", " man,", " boy", " male", " guy", " he ", " his ", " beard", " king", " prince", " father", " gentleman"})) return Gender::Masculine;
    return Gender::Unknown;
}

// a costume / fantasy / creature: its outfit is part of who it is, so it is adapted rather than replaced
bool LooksLikeCostume(const std::string& description) {
    return Has(Lower(description), {"knight", "armor", "armour", "mage", "wizard", "witch", "elf", "dragon", "robot", "alien", "samurai", "ninja", "superhero", "cape", "demon", "angel",
                                    "monster", "cyborg", "pirate", "viking", "uniform", "mascot", "costume", "cosplay", "paladin", "warrior", "orc", "goblin", "android"});
}

const std::vector<std::string>& OutfitList(const std::string& contextLow) {
    if (Has(contextLow, {"rain", "storm", "drizzle", "umbrella", "wet", "monsoon"})) return kRain;
    if (Has(contextLow, {"snow", "winter", "cold", "ice", "frozen", "blizzard", "ski"})) return kCold;
    if (Has(contextLow, {"beach", "summer", "pool", "tropical", "desert", "hot", "picnic", "festival", "sunflower", "sunny", "seaside"})) return kWarm;
    if (Has(contextLow, {"club", "party", "neon", "disco", "concert", "bar ", "arcade", "cyberpunk", "night market", "roller"})) return kNight;
    if (Has(contextLow, {"office", "meeting", "wedding", "gala", "restaurant", "court", "interview", "ballroom", "opera", "theatre", "formal", "reception"})) return kFormal;
    if (Has(contextLow, {"gym", "hike", "mountain", "forest", "running", "sport", "race", "climb", "skate", "track", "trail", "cycling", "go-kart", "camp"})) return kActive;
    if (Has(contextLow, {"home", "kitchen", "bedroom", "living room", "cafe", "café", "library", "bookshop", "sofa", "cabin", "fireplace", "attic", "farmhouse"})) return kCozy;
    return kGeneric;
}

std::string Pronounless(std::string s) { return s; }

} // namespace

// ---------------------------------------------------------------------------------------------------------------
std::vector<CharLook> DirectLooks(const std::vector<DirectorChar>& chars, const std::string& context, const DirectorFlags& flags, bool useClef, bool* clefUsed,
                                  const std::vector<std::string>* outfitList) {
    std::mt19937 rng(std::random_device{}());
    const std::string low = Lower(context);
    std::vector<Q> qs;
    std::vector<std::vector<std::string>> custom(chars.size()); // per character outfit list (kept alive for the questions)
    json state = {{"scene", context.empty() ? std::string("a scene with several characters") : context}, {"characters", json::array()}};
    for (const DirectorChar& c : chars) state["characters"].push_back({{"name", c.name}, {"description", c.description}});
    // the per-character questions: outfit (fresh or adapted), posture, expression
    std::vector<std::array<int, 3>> slot(chars.size(), {-1, -1, -1});
    for (size_t i = 0; i < chars.size(); ++i) {
        const bool adapt = flags.adaptOutfit || LooksLikeCostume(chars[i].description);
        const std::string who = chars[i].name + (chars[i].description.empty() ? std::string() : " (" + chars[i].description + ")");
        if (flags.outfit) {
            if (adapt) {
                custom[i] = kAdapt;
            } else {
                if (outfitList && !outfitList->empty()) {
                    custom[i] = *outfitList; // the costumes of the era
                } else {
                    custom[i] = OutfitList(low);
                    const Gender g = GuessGender(chars[i].description);
                    if (g == Gender::Feminine) custom[i].insert(custom[i].end(), kFeminine.begin(), kFeminine.end());
                    if (g == Gender::Masculine) custom[i].insert(custom[i].end(), kMasculine.begin(), kMasculine.end());
                }
            }
            slot[i][0] = static_cast<int>(qs.size());
            qs.push_back({"outfit" + std::to_string(i), adapt ? "How should " + who + "'s own outfit change to fit this moment?" : "Which outfit suits " + who + " best in this scene?", &custom[i]});
        }
        if (flags.pose) {
            slot[i][1] = static_cast<int>(qs.size());
            qs.push_back({"pose" + std::to_string(i), "Which body posture suits " + who + " best in this scene?", &kPose});
        }
        if (flags.expression) {
            slot[i][2] = static_cast<int>(qs.size());
            qs.push_back({"expr" + std::to_string(i), "Which facial expression suits " + who + " best in this scene?", &kExpression});
        }
    }
    bool used = false;
    const std::vector<int> picks = qs.empty() ? std::vector<int>() : ChooseAll(state, qs, useClef, rng, used);
    if (clefUsed) *clefUsed = used;
    std::vector<CharLook> out(chars.size());
    for (size_t i = 0; i < chars.size(); ++i) {
        if (slot[i][0] >= 0) out[i].outfit = (*qs[slot[i][0]].options)[picks[slot[i][0]]];
        if (slot[i][1] >= 0) out[i].pose = kPose[picks[slot[i][1]]];
        if (slot[i][2] >= 0) out[i].expression = kExpression[picks[slot[i][2]]];
    }
    return out;
}

std::string LooksClause(const std::vector<DirectorChar>& chars, const std::vector<CharLook>& looks, const DirectorFlags& flags) {
    std::string body;
    for (size_t i = 0; i < chars.size() && i < looks.size(); ++i) {
        const CharLook& l = looks[i];
        std::string parts;
        auto add = [&](const std::string& s) { parts += (parts.empty() ? "" : "; ") + s; };
        const bool adapt = flags.adaptOutfit || LooksLikeCostume(chars[i].description);
        if (!l.outfit.empty()) add(adapt ? "keeps their own outfit and style but adapted to the moment, with " + l.outfit : "wears " + l.outfit + " (not the clothes of the reference picture)");
        if (!l.pose.empty()) add("body: " + l.pose);
        if (!l.expression.empty()) add("expression: " + l.expression);
        if (parts.empty()) continue;
        body += (body.empty() ? "" : " ") + chars[i].name + (chars[i].tag.empty() ? std::string() : " (" + chars[i].tag + ")") + " " + parts + ".";
    }
    if (body.empty()) return {};
    return "Directions for each character (follow them exactly: they replace what the reference pictures show, and the body language must still serve what happens): " + body;
}

std::string RandomLookSentence(const std::string& description, const std::string& context) {
    DirectorChar c;
    c.name = "the person";
    c.description = description;
    DirectorFlags f;
    f.adaptOutfit = false;
    const std::vector<CharLook> l = DirectLooks({c}, context, f, true, nullptr);
    if (l.empty()) return {};
    return "wears " + l[0].outfit + "; body: " + l[0].pose + "; expression: " + l[0].expression;
}

// ---------------------------------------------------------------------------------------------------------------
std::vector<std::vector<double>> ComputeChemistry(const std::vector<DirectorChar>& pool) {
    const size_t n = pool.size();
    std::vector<std::vector<double>> m(n, std::vector<double>(n, 1.5));
    if (n < 3) return n == 2 ? m : std::vector<std::vector<double>>();
    static std::map<std::string, double> cache; // "nameA|descA#nameB|descB" -> score
    auto key = [&](size_t a, size_t b) { return pool[a].name + "|" + pool[a].description + "#" + pool[b].name + "|" + pool[b].description; };
    std::vector<ClefQuestion> qs;
    std::vector<std::pair<size_t, size_t>> which;
    for (size_t a = 0; a < n; ++a) {
        for (size_t b = a + 1; b < n; ++b) {
            auto it = cache.find(key(a, b));
            if (it != cache.end()) {
                m[a][b] = m[b][a] = it->second;
                continue;
            }
            ClefQuestion q;
            q.id = "p" + std::to_string(qs.size());
            q.type = ClefType::Score;
            q.instructions = "How well would " + pool[a].name + " (" + pool[a].description + ") and " + pool[b].name + " (" + pool[b].description + ") work together in one picture: contrast, chemistry, story potential?";
            q.options = {"they clash and nothing interesting comes of it", "an ordinary pairing", "good chemistry or a fun contrast", "a great duo with lots of story potential"};
            qs.push_back(std::move(q));
            which.push_back({a, b});
        }
    }
    for (size_t at = 0; at < qs.size(); at += 24) { // a few at a time: a request has to fit one batch
        const size_t end = std::min(qs.size(), at + 24);
        std::vector<ClefQuestion> part(qs.begin() + at, qs.begin() + end);
        std::vector<ClefAnswer> answers;
        if (!ClefAsk("Judge pairs of fictional characters for pictures of them together.", {}, part, answers, nullptr) || answers.size() != part.size()) return {};
        for (size_t i = 0; i < part.size(); ++i) {
            const auto [a, b] = which[at + i];
            m[a][b] = m[b][a] = answers[i].score;
            cache[key(a, b)] = answers[i].score;
        }
    }
    return m;
}

std::string Scenario::PlaceText() const {
    return place + (region.empty() ? std::string() : " in " + region) + (eraPhrase.empty() ? std::string() : ", " + eraPhrase);
}

std::string Scenario::Summary(const std::vector<DirectorChar>& pool) const {
    std::string who;
    for (size_t i = 0; i < cast.size(); ++i) {
        if (cast[i] >= 0 && cast[i] < static_cast<int>(pool.size())) who += (who.empty() ? "" : " + ") + pool[cast[i]].name;
    }
    std::string s = who + ": " + PlaceText() + " " + time;
    if (!relations.empty() && !relations[0].empty()) s += " | " + relations[0];
    return s;
}

namespace {

struct Cameras {
    std::vector<std::string> shots, angles, lights;
};

const Cameras& CameraLists() {
    static Cameras c = [] {
        Cameras k;
        for (size_t i = 1; i < CameraChoices(0).size(); ++i) k.shots.push_back(CameraChoices(0)[i].label);
        for (size_t i = 1; i < CameraChoices(1).size(); ++i) k.angles.push_back(CameraChoices(1)[i].label);
        for (size_t i = 1; i < CameraChoices(4).size(); ++i) k.lights.push_back(CameraChoices(4)[i].label);
        return k;
    }();
    return c;
}

std::string Describe(const Scenario& sc, const std::vector<DirectorChar>& pool) {
    std::string who;
    for (int i : sc.cast) {
        if (i >= 0 && i < static_cast<int>(pool.size())) who += (who.empty() ? "" : " and ") + pool[i].name;
    }
    std::string s = who + " in " + sc.PlaceText() + ", " + sc.time + ". ";
    if (!sc.genre.empty()) s += "Genre: " + sc.genre + ". ";
    if (!sc.relations.empty()) s += pool[sc.cast[0]].name + " " + sc.relations[0] + " " + pool[sc.cast[1]].name + ". ";
    s += "Mood: " + sc.mood + ". Detail: " + sc.twist + ". Object: " + sc.prop + ".";
    if (!sc.beat.empty()) s += " Moment: " + sc.beat + ".";
    return s;
}

// one candidate scenario (without the looks): the cast, then Clef (or chance) chooses everything else, in one request
Scenario Candidate(const std::vector<DirectorChar>& pool, int minChars, int maxChars, const std::vector<std::string>& relations, bool useClef, std::mt19937& rng, const StoryState* story,
                   bool& clefUsed, const std::vector<std::vector<double>>* chemistry) {
    Scenario sc;
    const int n = minChars + static_cast<int>(rng() % (maxChars - minChars + 1));
    if (story && !story->lastCast.empty() && rng() % 10 < 8) { // a story keeps (some of) its characters from scene to scene
        const int keep = story->lastCast[rng() % story->lastCast.size()];
        if (keep >= 0 && keep < static_cast<int>(pool.size())) sc.cast.push_back(keep);
    }
    if (sc.cast.empty()) sc.cast.push_back(static_cast<int>(rng() % pool.size()));
    while (static_cast<int>(sc.cast.size()) < n && sc.cast.size() < pool.size()) { // the others: the ones that work well with who is already in (Clef judged the pairs)
        std::vector<int> free;
        std::vector<double> w;
        for (size_t j = 0; j < pool.size(); ++j) {
            if (std::find(sc.cast.begin(), sc.cast.end(), static_cast<int>(j)) != sc.cast.end()) continue;
            double chem = 1.5;
            if (chemistry && chemistry->size() == pool.size()) {
                double sum = 0;
                for (int c : sc.cast) sum += (*chemistry)[c][j];
                chem = sum / sc.cast.size();
            }
            free.push_back(static_cast<int>(j));
            w.push_back(std::exp(1.1 * chem)); // 0 .. 3 -> about 1 .. 27 times as likely
        }
        std::discrete_distribution<int> pick(w.begin(), w.end());
        sc.cast.push_back(free[pick(rng)]);
    }
    json state = {{"task", story ? "Choose the next scene of an ongoing story of these characters" : "Invent a fun, surprising picture of these characters together"}, {"characters", json::array()}};
    for (int i : sc.cast) state["characters"].push_back({{"name", pool[i].name}, {"description", pool[i].description}});
    if (story && !story->summaries.empty()) {
        state["story_so_far"] = story->summaries;
        state["scene_number"] = story->step + 1;
    }
    // first: in which era / world, and the genre (a story stays in its world)
    const std::vector<Era>& eras = Eras();
    std::vector<std::string> eraOptions, genreOptions = kGenre;
    std::vector<int> eraOf;
    for (size_t e = 0; e < eras.size(); ++e) {
        const int copies = e == 0 ? 3 : 1; // the present day comes up more often
        for (int k = 0; k < copies; ++k) {
            eraOptions.push_back(e == 0 ? std::string("the present day") : std::string(eras[e].name) + " (" + eras[e].phrase + ")");
            eraOf.push_back(static_cast<int>(e));
        }
    }
    int eraIdx = 0;
    {
        std::vector<Q> aq = {{"genre", "What genre is the story?", &genreOptions}};
        bool keepEra = false;
        if (story && !story->lastEra.empty() && rng() % 100 < 85) {
            for (size_t e = 0; e < eras.size(); ++e) {
                if (story->lastEra == eras[e].name) {
                    eraIdx = static_cast<int>(e);
                    keepEra = true;
                }
            }
        }
        if (!keepEra) aq.push_back({"era", "In which era or world does the scene take place?", &eraOptions});
        bool used = false;
        const std::vector<int> ap = ChooseAll(state, aq, useClef, rng, used);
        clefUsed = clefUsed || used;
        sc.genre = kGenre[ap[0]];
        if (!keepEra) eraIdx = eraOf[ap[1]];
    }
    const Era& era = eras[eraIdx];
    sc.era = era.name;
    sc.eraPhrase = era.phrase;
    state["era"] = eraIdx == 0 ? std::string("the present day") : std::string(era.name);
    state["genre"] = sc.genre;
    static const std::vector<std::string> modernPlaces = [] {
        std::vector<std::string> v = kPlace;
        v.insert(v.end(), kPlaceMore.begin(), kPlaceMore.end());
        return v;
    }();
    const std::vector<std::string>& places = era.places.empty() ? modernPlaces : era.places;
    const bool askRegion = era.realWorld && (eraIdx == 0 || rng() % 100 < 45);
    const Cameras& cam = CameraLists();
    std::vector<Q> qs = {{"place", story ? "Where does the next scene happen?" : "Where should this scene happen?", &places},
                         {"time", "When and in what weather does it happen?", &kTime},
                         {"mood", "What kind of picture is it?", &kMood},
                         {"twist", "What surprising detail makes it memorable?", &kTwist},
                         {"prop", "Which object plays a part in the scene?", &kProp},
                         {"style", "How should it be photographed?", &kPhotoStyle}};
    if (askRegion) qs.push_back({"region", "In which country or city is it?", &kRegion});
    if (story) qs.push_back({"beat", "What moment of the story is this?", &kBeat});
    std::vector<std::string> rel = relations;
    if (sc.cast.size() >= 2) qs.push_back({"relation", "What are the first two characters doing together?", &rel});
    if (!cam.shots.empty()) qs.push_back({"shot", "How should the shot be framed?", &cam.shots});
    if (!cam.angles.empty()) qs.push_back({"angle", "From which camera angle?", &cam.angles});
    if (!cam.lights.empty()) qs.push_back({"light", "What lighting suits it?", &cam.lights});
    bool used2 = false;
    const std::vector<int> p = ChooseAll(state, qs, useClef, rng, used2);
    clefUsed = clefUsed || used2;
    size_t q = 0;
    sc.place = places[p[q++]];
    sc.time = kTime[p[q++]];
    sc.mood = kMood[p[q++]];
    sc.twist = kTwist[p[q++]];
    sc.prop = kProp[p[q++]];
    sc.photoStyle = kPhotoStyle[p[q++]];
    if (askRegion) sc.region = kRegion[p[q++]];
    if (story) sc.beat = kBeat[p[q++]];
    if (sc.cast.size() >= 2) {
        sc.relations.push_back(rel[p[q++]]);
        for (size_t i = 2; i < sc.cast.size(); ++i) sc.relations.push_back(PickOne(rel, rng)); // the others: chained to the previous one
    }
    if (!cam.shots.empty()) sc.shot = p[q++] + 1;
    if (!cam.angles.empty()) sc.angle = p[q++] + 1;
    if (!cam.lights.empty()) sc.lighting = p[q++] + 1;
    static const int shapes[] = {1, 1, 1, 4, 4, 4, 3, 3, 2, 5, 6};
    sc.canvas = shapes[rng() % (sizeof(shapes) / sizeof(shapes[0]))];
    if (sc.cast.size() >= 3 && (sc.canvas == 3 || sc.canvas == 5)) sc.canvas = 1; // groups need width
    return sc;
}

} // namespace

Scenario PlanScenario(const std::vector<DirectorChar>& pool, int minChars, int maxChars, const std::vector<std::string>& relationLabels, const DirectorFlags& flags, bool useClef, unsigned seed,
                      int bestOf, StoryState* story, const std::vector<std::vector<double>>* chemistry) {
    std::mt19937 rng(seed ? seed : std::random_device{}());
    Scenario sc;
    if (pool.empty()) return sc;
    minChars = std::clamp(minChars, 1, static_cast<int>(pool.size()));
    maxChars = std::clamp(maxChars, minChars, std::min(4, static_cast<int>(pool.size())));
    const std::vector<std::string> relations = relationLabels.empty() ? kRelation : relationLabels;

    // invent several candidates, and let Clef JUDGE them: a "how good is this idea" question for each, the best is kept
    const int tries = useClef ? std::clamp(bestOf, 1, 8) : 1;
    std::vector<Scenario> cands;
    bool clef1 = false;
    for (int t = 0; t < tries; ++t) {
        bool used = false;
        cands.push_back(Candidate(pool, minChars, maxChars, relations, useClef, rng, story, used, chemistry));
        clef1 = clef1 || used;
    }
    size_t best = 0;
    bool judged = false;
    if (tries > 1 && clef1) {
        std::vector<ClefQuestion> qs;
        static const std::vector<std::string> grades = {"makes no sense or is unpleasant", "boring and forgettable", "fun and interesting", "great: surprising, coherent and visual"};
        for (size_t t = 0; t < cands.size(); ++t) {
            ClefQuestion q;
            q.id = "idea" + std::to_string(t);
            q.instructions = std::string(story ? "How good is this as the next scene of the story? " : "How good is this idea for a picture? ") + Describe(cands[t], pool);
            q.options = grades;
            qs.push_back(std::move(q));
        }
        json state = {{"task", "Judge ideas for pictures of fictional characters: are they coherent, visual and fun?"}};
        if (story && !story->summaries.empty()) state["story_so_far"] = story->summaries;
        std::vector<std::vector<double>> probs;
        if (ClefDecide(state.dump(), qs, probs, nullptr) && probs.size() == cands.size()) {
            double top = -1;
            for (size_t t = 0; t < cands.size(); ++t) {
                double e = 0;
                for (size_t k = 0; k < probs[t].size(); ++k) e += probs[t][k] * static_cast<double>(k);
                cands[t].score = e;
                if (e > top) {
                    top = e;
                    best = t;
                }
            }
            judged = true;
        }
    }
    sc = cands[best];

    // looks, now that the place is known
    std::vector<DirectorChar> cast;
    for (int i : sc.cast) cast.push_back(pool[i]);
    const std::string context = sc.place + " " + sc.time + " " + sc.mood + " " + sc.twist + " " + sc.prop + " " + sc.region;
    bool clef2 = false;
    const std::vector<std::string>* eraOutfits = nullptr;
    for (const Era& e : Eras()) {
        if (sc.era == e.name && !e.outfits.empty()) eraOutfits = &e.outfits;
    }
    sc.looks = DirectLooks(cast, context, flags, useClef, &clef2, eraOutfits);
    sc.clefUsed = clef1 || clef2;
    sc.note = sc.clefUsed ? (judged ? "Clef chose and judged " + std::to_string(tries) + " ideas (this one scored " + std::to_string(sc.score).substr(0, 4) + " of 3)" : std::string("Clef chose"))
                          : (useClef ? "random (Clef did not answer)" : "random");
    if (story) {
        story->summaries.push_back(Describe(sc, pool));
        if (story->summaries.size() > 3) story->summaries.erase(story->summaries.begin());
        story->lastCast = sc.cast;
        story->lastEra = sc.era;
        ++story->step;
    }
    return sc;
}

const std::vector<std::string>& GenreNames() { return kGenre; }
const std::vector<std::string>& MoodNames() { return kMood; }
const std::vector<std::string>& EraNames() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> n;
        for (const Era& e : Eras()) n.push_back(n.empty() ? std::string("the present day") : std::string(e.name));
        return n;
    }();
    return v;
}
