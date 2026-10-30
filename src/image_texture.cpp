#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include "stb_image_write.h"

#include "image_texture.h"
#include "video_util.h"
#include "nsfw_classifier.h" // DecodeImageFileRGBA (stb + WIC fallback)
#include <algorithm>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <windows.h>

namespace {

bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "Could not open file";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (out.empty()) {
        if (error) *error = "Empty file";
        return false;
    }
    return true;
}

bool CreateTextureFromRGBA(ID3D11Device* device, const unsigned char* pixels, int width, int height,
                            LoadedImage& out, std::string* error) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sub = {};
    sub.pSysMem = pixels;
    sub.SysMemPitch = width * 4;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, &sub, &texture);
    if (FAILED(hr)) {
        if (error) *error = "Direct3D texture creation failed";
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* srv = nullptr;
    hr = device->CreateShaderResourceView(texture, &srvDesc, &srv);
    texture->Release();
    if (FAILED(hr)) {
        if (error) *error = "Shader resource view creation failed";
        return false;
    }

    out.srv = srv;
    out.width = width;
    out.height = height;
    return true;
}

// Area-averaging (box) downsample: much cleaner than nearest-neighbour for
// the 8-16x reductions gallery thumbnails need, and still a single pass.
std::vector<unsigned char> DownsampleRGBA(const unsigned char* src, int srcW, int srcH, int dstW, int dstH) {
    std::vector<unsigned char> dst(static_cast<size_t>(dstW) * dstH * 4);
    for (int y = 0; y < dstH; ++y) {
        int y0 = static_cast<int>(static_cast<long long>(y) * srcH / dstH);
        int y1 = std::max(y0 + 1, static_cast<int>(static_cast<long long>(y + 1) * srcH / dstH));
        for (int x = 0; x < dstW; ++x) {
            int x0 = static_cast<int>(static_cast<long long>(x) * srcW / dstW);
            int x1 = std::max(x0 + 1, static_cast<int>(static_cast<long long>(x + 1) * srcW / dstW));
            unsigned long long sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy) {
                const unsigned char* row = src + (static_cast<size_t>(sy) * srcW + x0) * 4;
                for (int sx = x0; sx < x1; ++sx, row += 4) {
                    sum[0] += row[0]; sum[1] += row[1]; sum[2] += row[2]; sum[3] += row[3];
                }
            }
            unsigned long long n = static_cast<unsigned long long>(y1 - y0) * (x1 - x0);
            unsigned char* d = dst.data() + (static_cast<size_t>(y) * dstW + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<unsigned char>((sum[c] + n / 2) / n);
        }
    }
    return dst;
}

std::string WideToUtf8Local(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

// Largest texture dimension every D3D11 feature level we create supports.
constexpr int kMaxTextureDim = 8192;

bool DecodeRGBAFromMemory(const unsigned char* bytes, size_t len, std::vector<unsigned char>& rgba, int& width,
                           int& height, std::string* error) {
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(bytes, static_cast<int>(len), &width, &height, &channels, 4);
    if (!pixels) {
        if (error) *error = stbi_failure_reason() ? stbi_failure_reason() : "Unknown decode error";
        return false;
    }
    rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

// stb first (fast path for PNG/JPEG), then Windows Imaging Component for
// everything else it can't read (WebP, ...).
bool DecodeFile(const std::wstring& path, std::vector<unsigned char>& rgba, int& width, int& height,
                std::string* error) {
    if (IsVideoPath(path)) return DecodeVideoFrameRGBA(path, rgba, width, height); // never read a whole video into memory
    std::vector<unsigned char> buf;
    if (!ReadFileBytes(path, buf, error)) return false;
    std::string stbError;
    if (DecodeRGBAFromMemory(buf.data(), buf.size(), rgba, width, height, &stbError)) return true;
    if (DecodeImageFileRGBA(path, rgba, width, height)) return true;
    if (error) *error = "Unsupported or corrupt image (" + stbError + ")";
    return false;
}

// Uploads, first shrinking images beyond the GPU's texture limit.
bool UploadRGBA(ID3D11Device* device, std::vector<unsigned char>& rgba, int width, int height, LoadedImage& out,
                std::string* error) {
    if (width > kMaxTextureDim || height > kMaxTextureDim) {
        float scale = static_cast<float>(kMaxTextureDim) / static_cast<float>(std::max(width, height));
        int dstW = std::max(1, static_cast<int>(width * scale));
        int dstH = std::max(1, static_cast<int>(height * scale));
        rgba = DownsampleRGBA(rgba.data(), width, height, dstW, dstH);
        width = dstW;
        height = dstH;
    }
    return CreateTextureFromRGBA(device, rgba.data(), width, height, out, error);
}

} // namespace

bool DecodeImageRGBA(const std::wstring& path, std::vector<unsigned char>& rgba, int& width, int& height, std::string* error) {
    return DecodeFile(path, rgba, width, height, error);
}

bool CreateImageFromRGBA(ID3D11Device* device, const unsigned char* rgba, int width, int height, LoadedImage& out, std::string* error) {
    if (!device || !rgba || width <= 0 || height <= 0) return false;
    return CreateTextureFromRGBA(device, rgba, width, height, out, error);
}

bool LoadImageTexture(ID3D11Device* device, const std::wstring& path, LoadedImage& out, std::string* error) {
    std::vector<unsigned char> rgba;
    int width = 0, height = 0;
    if (!DecodeFile(path, rgba, width, height, error)) return false;
    return UploadRGBA(device, rgba, width, height, out, error);
}

bool LoadImageTextureFromMemory(ID3D11Device* device, const unsigned char* data, size_t len, LoadedImage& out,
                                 std::string* error, std::vector<unsigned char>* outRawRGBA) {
    std::vector<unsigned char> rgba;
    int width = 0, height = 0;
    if (!DecodeRGBAFromMemory(data, len, rgba, width, height, error)) return false;
    const bool ok = UploadRGBA(device, rgba, width, height, out, error);
    if (ok && outRawRGBA) *outRawRGBA = std::move(rgba);
    return ok;
}

bool LoadImageThumbnail(ID3D11Device* device, const std::wstring& path, int maxDim, LoadedImage& out,
                         std::vector<unsigned char>* outRawRGBA, std::string* error) {
    std::vector<unsigned char> rgba;
    int width = 0, height = 0;
    if (!DecodeFile(path, rgba, width, height, error)) return false;

    if (width > maxDim || height > maxDim) {
        float scale = static_cast<float>(maxDim) / static_cast<float>(std::max(width, height));
        int dstW = std::max(1, static_cast<int>(width * scale));
        int dstH = std::max(1, static_cast<int>(height * scale));
        rgba = DownsampleRGBA(rgba.data(), width, height, dstW, dstH);
        width = dstW;
        height = dstH;
    }
    if (!CreateTextureFromRGBA(device, rgba.data(), width, height, out, error)) return false;
    if (outRawRGBA) *outRawRGBA = std::move(rgba);
    return true;
}

bool WriteThumbnailPng(const std::wstring& path, const unsigned char* rgba, int width, int height) {
    std::string utf8Path = WideToUtf8Local(path);
    return stbi_write_png(utf8Path.c_str(), width, height, 4, rgba, width * 4) != 0;
}

void ReleaseImage(LoadedImage& img) {
    if (img.srv) {
        ReleaseAfterFrame(img.srv);
        img.srv = nullptr;
    }
    img.width = 0;
    img.height = 0;
}

// ---- let go once the frame is on screen. A texture released in the middle of a frame (a clip switched under the video it was showing, a
// picture replaced) can already be in that frame's draw list: freed at once, ImGui then bound a dead view and the NVIDIA driver's worker
// thread crashed on it later (nvwgf2umx reading 0x10, 2026-10-26, a clip opening on the Video page; the same as the October crashes).
namespace {
std::mutex g_laterMu;
std::vector<IUnknown*> g_later;
} // namespace

void ReleaseAfterFrame(IUnknown* p) {
    if (!p) return;
    std::lock_guard<std::mutex> lock(g_laterMu);
    g_later.push_back(p);
}

void ReleaseFrameLeftovers() {
    std::vector<IUnknown*> now;
    {
        std::lock_guard<std::mutex> lock(g_laterMu);
        now.swap(g_later);
    }
    for (IUnknown* p : now) p->Release();
}

// ---- the NSFW blur: a soft copy of a texture, made from a tiny version of it (drawn stretched, the linear sampler smooths it)
namespace {
struct BlurredEntry {
    ID3D11ShaderResourceView* blurred = nullptr;
    int lastFrame = 0;
};
std::unordered_map<ID3D11ShaderResourceView*, BlurredEntry> g_blurred; // the key holds a reference: its address cannot be reused meanwhile
int g_blurFrame = 0, g_blurMadeFrame = -1, g_blurMadeCount = 0;

ID3D11ShaderResourceView* MakeBlurred(ID3D11Device* device, ID3D11ShaderResourceView* src) {
    ID3D11Resource* res = nullptr;
    src->GetResource(&res);
    if (!res) return nullptr;
    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    res->Release();
    if (!tex) return nullptr;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
    const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const int tinySide = 20; // the longer side of the tiny version
    const int sw = d.Width >= d.Height ? tinySide : std::max(2, static_cast<int>(tinySide * static_cast<float>(d.Width) / std::max(1u, d.Height)));
    const int sh = d.Height > d.Width ? tinySide : std::max(2, static_cast<int>(tinySide * static_cast<float>(d.Height) / std::max(1u, d.Width)));
    std::vector<float> acc(static_cast<size_t>(sw) * sh * 4, 0.0f);
    std::vector<float> cnt(static_cast<size_t>(sw) * sh, 0.0f);
    bool read = false;
    if ((bgra || rgba) && d.Width > 0 && d.Height > 0 && d.SampleDesc.Count == 1) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        device->GetImmediateContext(&ctx);
        if (ctx && SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &staging))) {
            ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                const int step = std::max(1, static_cast<int>(std::max(d.Width, d.Height) / 160)); // (every few pixels of a big picture is plenty)
                for (UINT y = 0; y < d.Height; y += step) {
                    const unsigned char* row = static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                    const int ty = std::min(sh - 1, static_cast<int>(static_cast<float>(y) * sh / d.Height));
                    for (UINT x = 0; x < d.Width; x += step) {
                        const unsigned char* px = row + static_cast<size_t>(x) * 4;
                        const int tx = std::min(sw - 1, static_cast<int>(static_cast<float>(x) * sw / d.Width));
                        float* a = &acc[(static_cast<size_t>(ty) * sw + tx) * 4];
                        a[0] += bgra ? px[2] : px[0];
                        a[1] += px[1];
                        a[2] += bgra ? px[0] : px[2];
                        a[3] += d.Format == DXGI_FORMAT_B8G8R8X8_UNORM ? 255.0f : px[3];
                        cnt[static_cast<size_t>(ty) * sw + tx] += 1.0f;
                    }
                }
                ctx->Unmap(staging, 0);
                read = true;
            }
            staging->Release();
        }
        if (ctx) ctx->Release();
    }
    tex->Release();
    std::vector<float> img(static_cast<size_t>(sw) * sh * 4, 60.0f); // (a format it cannot read: plain grey)
    if (read) {
        for (size_t i = 0; i < cnt.size(); ++i)
            for (int c = 0; c < 4; ++c) img[i * 4 + c] = cnt[i] > 0 ? acc[i * 4 + c] / cnt[i] : 0.0f;
    }
    for (int pass = 0; pass < 2; ++pass) { // softened further: a 3x3 average, twice
        std::vector<float> out(img.size());
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x)
                for (int c = 0; c < 4; ++c) {
                    float s = 0, n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = std::clamp(x + dx, 0, sw - 1), yy = std::clamp(y + dy, 0, sh - 1);
                            s += img[(static_cast<size_t>(yy) * sw + xx) * 4 + c];
                            n += 1;
                        }
                    out[(static_cast<size_t>(y) * sw + x) * 4 + c] = s / n;
                }
        img.swap(out);
    }
    std::vector<unsigned char> px(img.size());
    for (size_t i = 0; i < img.size(); ++i) px[i] = static_cast<unsigned char>(std::clamp(i % 4 == 3 ? img[i] : img[i] * 0.85f, 0.0f, 255.0f)); // a little darker
    LoadedImage out;
    if (!CreateImageFromRGBA(device, px.data(), sw, sh, out, nullptr)) return nullptr;
    return out.srv;
}
} // namespace

ID3D11ShaderResourceView* BlurredCopy(ID3D11Device* device, ID3D11ShaderResourceView* src) {
    if (!device || !src) return nullptr;
    auto it = g_blurred.find(src);
    if (it != g_blurred.end()) {
        it->second.lastFrame = g_blurFrame;
        return it->second.blurred;
    }
    if (g_blurMadeFrame != g_blurFrame) g_blurMadeFrame = g_blurFrame, g_blurMadeCount = 0;
    if (++g_blurMadeCount > 12) return nullptr; // a few a frame: a page full of them does not stall
    ID3D11ShaderResourceView* b = MakeBlurred(device, src);
    if (!b) return nullptr;
    src->AddRef();
    g_blurred[src] = {b, g_blurFrame};
    return b;
}

void PruneBlurredCopies() {
    ++g_blurFrame;
    if (g_blurFrame % 120 != 0) return;
    for (auto it = g_blurred.begin(); it != g_blurred.end();) {
        if (g_blurFrame - it->second.lastFrame > 600) { // not drawn for a while (about 10 s)
            it->second.blurred->Release();
            it->first->Release();
            it = g_blurred.erase(it);
        } else {
            ++it;
        }
    }
}

void ReleaseBlurredCopies() {
    for (auto& [src, e] : g_blurred) {
        e.blurred->Release();
        src->Release();
    }
    g_blurred.clear();
}
