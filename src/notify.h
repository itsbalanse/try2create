#pragma once
// Toasts: cards in the bottom-right corner when something finishes (a ComfyUI job, a design, pixel art) or fails. A card shows a title, a line, the
// result's picture, and opens the result when clicked.
#include <functional>
#include <string>

class AppState;

enum class ToastKind { Info, Success, Error };

// `key`: a toast with the same key replaces the old one (a job that finishes after it was sent). `crisp`: the picture is pixel art (no smoothing).
void Toast(ToastKind kind, const std::string& title, const std::string& body, const std::wstring& image = {}, std::function<void(AppState&)> open = {},
           const std::string& key = {}, bool crisp = false);
void DrawToasts(AppState& app);     // once a frame, after everything else
void WatchComfyJobs(AppState& app); // ComfyUI jobs that finish become toasts (the tabs that announce their own are left out)
bool ToastsShowing();               // the window keeps drawing while they fade
