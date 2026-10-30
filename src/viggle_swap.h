#pragma once
// Star in a video's second way to swap someone (viggle_swap.cpp): Viggle-Animate, from one of the clip's own frames with the new person
// painted in (FLUX 3 Klein does the painting), carried through the whole clip - no mask, no grey figure, no prompt.
#include <string>
#include <vector>

#include "json.hpp"

bool ViggleReady(std::string* missing);   // its model, LoRA, text conditioning and node pack are where ComfyUI looks ("" missing = ready)
// The swap: "@clip" is the cut part (its own sound goes back on), "@pic1" the painted frame; w x h on the 32 grid; over 124 frames the
// windowed planner + chunked sampler (124-frame windows, five-frame anchors). vedaPredictor "" = ComfyUI's own sol-attn sparse attention
nlohmann::json BuildViggleSwap(int w, int h, int frames, const std::string& outName, const std::string& vedaPredictor);
void ViggleSize(int clipW, int clipH, double megapixels, int& w, int& h); // the clip's shape at about this many megapixels (32 grid)
// One FLUX 3 edit that paints one person in (image 1 the frame, image 2 their picture): `who` is whom they take the place of. Several people
// are painted one edit after the other, each on the one before - all of them in one edit (three pictures) changed nothing (2026-10-29).
// `looks`: who they are and how they look, in words (the writer's look at their picture, or the character's own description) - without it
// FLUX 3 mostly left realistic people as they were (2026-10-29: 4 of 5 shots unchanged; named and described, see the README)
std::string ViggleRepaintPrompt(const std::string& who, const std::string& looks);
// The writer's own edit for it, looking at the frame and their picture: part by part what becomes what ("her black t-shirt becomes the white
// strapless wedding dress"), which is what FLUX 3 needs - a fixed wording worked on some frames and left others unchanged (2026-10-29: the
// same Peach on a full-body dance and a close-up). "" = no writer / no answer (then ViggleRepaintPrompt). Blocks.
std::string ViggleWriteEdit(const std::wstring& frame, const std::wstring& picture, const std::string& who, const std::string& looks, const std::string& comfyUrl);
// The repaint with MOSS-Image instead (it takes no picture: words only, and it is less held back): SAM 3 finds `find` in "@frame", only
// there MOSS (SFT, its Best settings) redraws from `prompt`, the rest is pasted back as it was. Tested 2026-10-29: a proper Bowser on the jump
// dancer (FLUX 3 had made an odd one from a wrong name), but Peach came out as a dress with no head three times - so it is a choice, not the default.
nlohmann::json BuildMossRepaint(const std::string& find, const std::string& prompt);
// The writer's prompt for that: the character in words (MOSS cannot see their picture) in the pose and place of the frame. "" = no writer.
std::string ViggleWriteMoss(const std::wstring& frame, const std::wstring& picture, const std::string& who, const std::string& looks, const std::string& comfyUrl);
// Its set-up (tools\viggle\setup_viggle.ps1: about 21 GB of files and the node pack), started by your click
void StartViggleSetup();
bool ViggleSetupRunning();
std::string ViggleSetupLine(); // its last line ("" = not run)
