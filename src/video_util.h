#pragma once
// Video files in the gallery: a poster frame (Windows Media Foundation, nothing to install), the clip's
// length and size, and the ComfyUI workflow / prompt that generators embed in the file's metadata.
#include <string>
#include <vector>
#include "metadata.h"

bool IsVideoPath(const std::wstring& path);

// Decodes one frame a little way into the clip as RGBA. Never fails for a video path: when the codec
// isn't available a plain placeholder picture is returned and `decoded` (if given) is false.
bool DecodeVideoFrameRGBA(const std::wstring& path, std::vector<unsigned char>& rgba, int& width, int& height,
                          bool* decoded = nullptr);

// Length in seconds and frame size (0 when unknown).
struct VideoInfo {
    double seconds = 0;
    int width = 0, height = 0;
    double fps = 0; // only known from the ffmpeg probe
};
VideoInfo ProbeVideo(const std::wstring& path);

// "workflow" and "prompt" JSON documents found in the file (MP4/MOV atoms, WebM/MKV tags).
std::vector<RawField> ReadVideoWorkflowFields(const std::wstring& path);

std::string FormatDuration(double seconds); // 0:05, 1:23, 1:02:03

// ffmpeg.exe, when it is on the PATH (empty otherwise), and its probe of a file (length, size, frame rate).
std::wstring FfmpegPath();
VideoInfo ProbeVideoFfmpeg(const std::wstring& path);
// Runs a command line without a window and collects its stdout (and stderr when `mergeStderr`).
bool RunCaptureHidden(const std::wstring& cmdline, std::string& out, bool mergeStderr);
// The same at idle CPU priority: background clean-up work that must never make the PC lag.
bool RunCaptureIdle(const std::wstring& cmdline, std::string& out, bool mergeStderr);
