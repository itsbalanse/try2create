# Try2Create - guide for Claude Code (and anyone changing the code)

This file is for AI coding assistants and people working on the code. Open this folder in Claude Code and ask for what you want ("add a
picture recipe that makes a sticker", "remove the Drama page", "make the Gallery thumbnails bigger"); this page tells it how the app is built.

## What it is

A native Windows desktop app (C++17, Dear ImGui on Direct3D 11, Win32; one exe, no Electron or web UI) that is a friendly front end for
[ComfyUI](https://github.com/comfyanonymous/ComfyUI). The user presses a button; the app builds a ComfyUI workflow in code (API-format JSON),
sends it to ComfyUI over HTTP, follows the job and shows the result. It also reads the prompts and settings stored inside AI pictures, and
runs a few small local models itself: llama.cpp for the prompt writer and Clef mode, ONNX Runtime + DirectML for the NSFW, quality and
person classifiers, ffmpeg for video cuts and joins.

## Build and run

- Needs Visual Studio 2022 (or its Build Tools) with *Desktop development with C++*.
- `build.bat` (incremental) or `build.bat clean`: CMake + NMake into `build-nmake/`. The exe is `build-nmake/bin/Try2Create.exe`, and the
  script copies it next to itself with the ONNX Runtime DLLs. If the app is running, that copy fails; the build itself is still fine.
- GitHub Actions (`.github/workflows/build.yml`) builds every push and makes a zip; a `v*` tag publishes it as a release.
- There is no unit test suite. Check a change by building it and running the app (see *Testing* below).

## Where things are (src/)

| File | What it holds |
|---|---|
| `main.cpp` | The window, the D3D11 device, the frame loop, menus, the sidebar of pages, the status bar (with the ComfyUI job tracker), the Ctrl+K command palette, the welcome screen. Page indices: 0 Gallery, 1 Vision library, 2 Workflows, 3 Vision board, 4 Generate, 5 Characters, 6 Edits, 8 Clef mode, 9 Pixel art, 10 Video, 11 Music/Sound, 12 Drama, 13 LoRAs. |
| `app_state.*` | The Gallery's data: folders, entries, thumbnails (disk cache), NSFW scores, ratings, selection. |
| `metadata*.cpp`, `metadata_parsers.cpp` | Reading prompts and settings from PNG / JPEG / WebP (A1111, ComfyUI graphs, NovelAI, InvokeAI...). |
| `comfy.*` | The ComfyUI client: finding ComfyUI (Desktop's install, a portable folder, an address), starting it in the background, HTTP, the job queue (`ComfyService`), uploads, results, and several built-in workflows (picture edits, upscaling). |
| `comfy_ui.cpp` | The Generate page's ComfyUI parts, the results viewer, Options, the status-bar job tracker. |
| `generate_ui.cpp` | Generate: the prompt box and which model makes it (Krea 2, Qwen Image 2.1, MOSS, your own workflow, video). |
| `edits_ui.cpp` | Edits: its start screen and modes; the list of picture edits. Modes: `apps_ui.cpp` (one-click recipes), `layout_ui.cpp` (MOSS designs and box edits), `design_ui.cpp` (PhotoCraft text layers), `upscale_ui.cpp`. |
| `video_ui.cpp` | Video: MiniMax H3 / LTX workflows (Animate a picture, long videos, Star in a video / person swap, Recreate, Make it crisp), the player, the videos grid. `video_tools.cpp`: background removal, transitions, auto-edit. `story_ui.cpp`: Make a story. |
| `music_ui.cpp`, `drama_ui.cpp` | Sound: songs (MiniMax Music) and multi-voice scenes (Fish Audio Drama). |
| `characters_ui.cpp`, `character_creator.cpp`, `character_story.cpp` | Characters, their creator and backstories. |
| `board*.cpp` | The Vision board. |
| `clef*.cpp` | The local language models through llama.cpp: Clef mode, the prompt writer (`WriterChat`), embeddings. |
| `pixel_*.cpp`, `pf_*.cpp` | Pixel art and its pixel fixer. |
| `setup_ui.cpp` | The first-run setup: the PC (VRAM/RAM), ComfyUI, model packs, ffmpeg and the prompt writer downloads. |
| `preset_art.cpp` | The drawn art on the Video page's idea cards and the Edits start screen (ImGui draw lists, no image files). |
| `nsfw_classifier.*`, `aesthetic_scorer.*`, `person_detector.*` | The ONNX models (in `models/`, downloaded, not in git). |
| `ui_theme.*`, `notify.*`, `ui_extra.h` | Colours and helpers (`ui::Dp`, `ui::PrimaryButton`, `ui::SectionHeading`), the corner toasts, declarations shared between pages. |

## How the code is written

- **Workflows are built in code**, as `nlohmann::json` with numbered nodes (`node("12", "KSamplerSelect", {...})`). ComfyUI nodes are
  referred to by their class names; models by the file names Comfy-Org publishes (for example `krea2_turbo_int8_convrot.safetensors`).
  A model that might be missing is looked for in ComfyUI's `/object_info` (`PickChoice`) and the error says exactly which file goes where.
- **Nothing heavy is in git.** Models are downloaded into ComfyUI's models folder by the setup or a page's *Set up* button (curl, resumable).
  Tools the app runs (llama.cpp, ffmpeg) go in `%LOCALAPPDATA%\Try2Create\tools`.
- **App data** (settings, caches, crash reports) is in `%LOCALAPPDATA%\Try2Create`.
- **Comments say why**, often with the test that decided it (times on an RTX 3090, what went wrong before). Keep that style: a short
  reason and the measurement, not a description of what the next line does.
- **Each page keeps its state** in a file-local struct (`g_v`, `g_s`...) inside an anonymous namespace; things other files need are declared
  in `ui_extra.h` (or the page's own header).
- **The UI speaks plain English** to someone who has never seen ComfyUI: say what a button does and what it costs ("about 3 min", "13.5 GB").
  Long jobs never block the UI: a worker thread or `std::async`, polled once a frame.
- **Hardware differs.** Read the GPU's memory and the RAM (`setup_ui.cpp`, `ReadPc`) instead of assuming 24 GB. Downloads say their size first.

## Testing a change

- Run the built exe. To keep your own settings out of it, give the test copy its own data folder: start it with `LOCALAPPDATA` set to an
  empty folder (that is what a brand-new user looks like).
- Test hooks (environment variables read at start-up) open pages and press buttons where a mouse cannot reach: `MV_UI_TEST` takes
  `;`-separated words such as `tab:N` (open page N), `setup` / `setup:N` (the setup at step N), `videopage:N`, `draw` (keep drawing while
  the window is hidden); `MV_SETUP_CLICK="Next|Next|Download the picked ones"` presses setup buttons in order; `MV_EDITS_MODE=N` opens an
  Edits mode; `MV_CRISP="<video>|1440"` runs Make it crisp on a file. Search the code for `MV_` to find the rest.
- Crash reports (`crash_*.txt` with a stack, and a `.dmp`) are in `%LOCALAPPDATA%\Try2Create\crash`.

## Licences

The code is MIT (`LICENSE`); bundled libraries are listed in `THIRD_PARTY_NOTICES.txt`. Models keep their own licences (shown in the setup
next to each pack); never commit model files or tool binaries.
