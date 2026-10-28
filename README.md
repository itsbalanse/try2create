# Try2Create

A Windows app that puts a friendly face on local AI. You press a button; ComfyUI does the work behind it, so you never have to wire a
workflow. Pictures, edits, videos with sound, music, characters and pixel art, all running on your own GPU.

![Try2Create](docs/screenshots/try2create_showcase.jpg)

> **Status: early.** It grew as one person's app, tuned on one PC (Windows 11, RTX 3090 24 GB, 128 GB RAM). This repository is the public
> edition: the aim is for it to set itself up on other PCs too. Expect rough edges, and please open an issue when something doesn't work on
> yours.

## What it does

| | |
|---|---|
| **Gallery** | Open a folder and read the prompt, settings, LoRAs and tags inside every picture (ComfyUI, AUTOMATIC1111, NovelAI, InvokeAI...). Search by words or by meaning, filters, ratings, duplicate hiding, an NSFW blur. |
| **Generate** | One prompt box. It picks the model (Krea 2 for people and most pictures, Qwen Image for creatures and extreme looks, MOSS-Image for exact text), or runs your own ComfyUI workflow. |
| **Edits** | One-click apps (relight, new angle, plushie, product shot...), "change anything" in words (Qwen Image Edit), designs with exact text and box edits (MOSS-Image), text layers over a picture (PhotoCraft), upscaling, and making videos crisp. |
| **Video with sound** | Animate a picture with MiniMax H3: idea cards and camera moves, a local LLM writes the prompt. Long videos in parts, swapping a person into a clip, short stories, recreating a video's settings, and *Make it crisp* (Open-H3-Regenerate) to 2K. |
| **Sound** | Songs with lyrics (MiniMax Music) and multi-voice audio scenes (Fish Audio Drama 3). |
| **Characters & story** | Characters with backstories and sheets, a 360-degree turntable, a vision board, Clef mode (a scenario that keeps going picture after picture), pixel art made into real pixels. |

More screenshots are in [docs/screenshots](docs/screenshots).

## What you need

- Windows 10 or 11, 64-bit
- [ComfyUI](https://www.comfy.org/download) (ComfyUI Desktop is found by itself; a portable ComfyUI or another address can be set in *Generate > ComfyUI*)
- An NVIDIA GPU. Pictures and edits are fine on 12-16 GB; the video features want 24 GB.
- Plenty of RAM for video (ComfyUI holds big models in RAM: 64 GB or more for MiniMax H3)
- The models: each feature has a *Set up* button that downloads what it needs, with its size, when you first use it. Models come from
  their publishers under their own licences; none are in this repository.

## Getting started

1. Download `Try2Create.zip` from the latest [release](../../releases), unzip it anywhere and run `Try2Create.exe`.
2. **Set up Try2Create** opens by itself the first time. It looks at your graphics card and RAM and says what will run well; finds ComfyUI
   (or helps you install ComfyUI Desktop, or use a portable ComfyUI or one at another address); then offers the models as packs (Pictures,
   Edits, Designs, Video, Music) with their sizes, ticking only what fits your PC. Only ComfyUI is required: skip any pack or helper and add
   it later from *Help > Set up Try2Create*.
3. The Gallery works straight away: open a folder of AI pictures to read their prompts and settings.

## Changing it with Claude Code (or any AI assistant)

The code is meant to be changed by asking. Clone the repo, open the folder in [Claude Code](https://claude.com/claude-code) and say what you
want: "add a one-click app that turns a photo into a sticker", "remove the Drama page", "make Generate default to square pictures".
[CLAUDE.md](CLAUDE.md) explains how the app is built, where each page lives, the conventions and how to test, so the assistant starts with
the full picture. Run `build.bat` to try the result.

## Building it

Install Visual Studio 2022 (or its Build Tools) with *Desktop development with C++*, then:

```bat
build.bat
```

`Try2Create.exe` lands next to the script, with the ONNX Runtime DLLs it needs. `package.bat` makes a zip to share. Every push to `main`
is also built by GitHub Actions (see *Actions* for the latest `Try2Create` zip).

## Contributing

Ideas, bug reports and pull requests are welcome. The parts that need the most help:

- **Setting itself up on any PC**: finding ComfyUI wherever it is, picking models that fit the GPU's memory, a first-run guide.
- **Smaller GPUs**: lighter model choices (GGUF, fp8) for 8-16 GB cards.
- **Testing** on hardware other than an RTX 3090.

## Licence

The app's code is under the [MIT licence](LICENSE). Libraries it bundles and their licences are listed in
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt). AI models are downloaded separately and each comes with its own licence.
