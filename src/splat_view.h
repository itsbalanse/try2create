// A 3D Gaussian splat viewer on the app's own Direct3D 11 device: reads a standard 3DGS .ply and draws it in an ImGui region, orbit with the mouse.
#pragma once
#include <string>

#include "imgui.h"

struct ID3D11Device;

namespace splat {

void Open(const std::wstring& ply);  // starts reading it (the view keeps the old one until it is there)
void Close();
bool Reading();
int Count();                         // Gaussians shown (0 = none)
const std::wstring& Path();
const std::string& Error();
bool Spinning();                     // the view turns by itself: the app keeps drawing

// The view, filling `size`: left drag orbits, right or middle drag moves, the wheel zooms, a double click goes back to the start
void Draw(ID3D11Device* device, ImVec2 size);
void Release();                      // the GPU buffers, when the app closes

extern bool g_spin;                  // turn by itself
extern bool g_flip;                  // upside down (the file's up is -y by default, as the 3DGS files are)

} // namespace splat
