// A 3D Gaussian splat viewer (2026-10-30, for the 3D page): instanced quads, each Gaussian's 3D covariance projected to a 2D ellipse in the vertex
// shader, drawn back to front with premultiplied alpha into a texture the page shows. The order is a 16-bit counting sort on the CPU, redone only
// when the camera moved. Only the 0th-order colour is used (f_dc: Idol writes no others).
#include "splat_view.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <sstream>
#include <vector>

#include "json.hpp"
#include "ui_theme.h"

namespace splat {

bool g_spin = false;
bool g_flip = true;

namespace {

struct Gpu { // (the shader's Splat)
    float pos[3], alpha;
    float color[3], pad;
    float cxx, cxy, cxz, cyy;
    float cyz, czz, pad2[2];
};
static_assert(sizeof(Gpu) == 64, "the shader reads 64 bytes");

struct Cloud {
    std::vector<Gpu> splats;
    float center[3] = {0, 0, 0};
    float radius = 1.0f;
    float yaw = 0.0f, pitch = 0.08f; // where the view starts
    std::string error;
};

std::future<Cloud> g_reading;
std::wstring g_readingPath, g_path;
std::string g_error;

std::vector<Gpu> g_cloud;
float g_center[3] = {0, 0, 0}, g_radius = 1.0f, g_startYaw = 0.0f, g_startPitch = 0.08f;
bool g_upload = false;

// the camera
float g_yaw = 0.0f, g_pitch = 0.1f, g_dist = 3.0f, g_pan[3] = {0, 0, 0};
float g_lastView[9] = {};
bool g_sortNeeded = true;

// GPU
ID3D11Device* g_dev = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11Buffer* g_splats = nullptr;
ID3D11Buffer* g_order = nullptr;
ID3D11ShaderResourceView* g_splatsSrv = nullptr;
ID3D11ShaderResourceView* g_orderSrv = nullptr;
ID3D11Texture2D* g_target = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11ShaderResourceView* g_targetSrv = nullptr;
int g_tw = 0, g_th = 0, g_gpuCount = 0;
std::vector<uint32_t> g_sorted, g_keys, g_counts;

template <class T>
void Drop(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

const char* const kShader = R"(
struct Splat { float3 pos; float alpha; float3 color; float pad; float cxx, cxy, cxz, cyy; float cyz, czz; float2 pad2; };
StructuredBuffer<Splat> splats : register(t0);
StructuredBuffer<uint> order : register(t1);
cbuffer View : register(b0) { float4 row0; float4 row1; float4 row2; float2 focal; float2 viewport; };
struct V { float4 pos : SV_Position; float2 q : TEXCOORD0; float4 col : COLOR0; };
V vs(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    V o;
    Splat s = splats[order[inst]];
    float4 p = float4(s.pos, 1);
    float3 c = float3(dot(row0, p), dot(row1, p), dot(row2, p)); // camera space: x right, y up, z forward
    o.q = float2(0, 0);
    o.col = float4(0, 0, 0, 0);
    o.pos = float4(0, 0, 2, 1);
    if (c.z < 0.02) return o;
    float3x3 R = float3x3(row0.xyz, row1.xyz, row2.xyz);
    float3x3 Cw = float3x3(s.cxx, s.cxy, s.cxz, s.cxy, s.cyy, s.cyz, s.cxz, s.cyz, s.czz);
    float3x3 Cc = mul(R, mul(Cw, transpose(R)));
    float iz = 1.0 / c.z;
    float2x3 J = float2x3(focal.x * iz, 0, -focal.x * c.x * iz * iz, 0, focal.y * iz, -focal.y * c.y * iz * iz); // (the projection, near this point)
    float2x2 S = mul(J, mul(Cc, transpose(J))); // pixels squared
    float a = S._11, b = S._12, d = S._22, tr = (a + d) * 0.5, del = sqrt(max(0.0, (a - d) * (a - d) * 0.25 + b * b));
    float l1 = tr + del, l2 = max(tr - del, 0.0);
    float2 v1 = abs(b) > 1e-7 ? normalize(float2(b, l1 - a)) : (a >= d ? float2(1, 0) : float2(0, 1));
    float2 v2 = float2(-v1.y, v1.x);
    float2 ax1 = min(sqrt(2.0 * l1), 1024.0) * v1, ax2 = min(sqrt(2.0 * l2), 1024.0) * v2;
    float2 q = float2((vid & 1) ? 2.0 : -2.0, (vid & 2) ? 2.0 : -2.0);
    float2 px = float2(focal.x * c.x * iz, focal.y * c.y * iz) + q.x * ax1 + q.y * ax2;
    o.pos = float4(px / (viewport * 0.5), 0.5, 1);
    o.q = q;
    o.col = float4(s.color, s.alpha);
    return o;
}
float4 ps(V i) : SV_Target {
    float A = -dot(i.q, i.q);
    if (A < -4.0) discard;
    float B = exp(A) * i.col.a;
    return float4(i.col.rgb * B, B);
}
)";

ID3DBlob* Compile(const char* entry, const char* target, std::string& error) {
    ID3DBlob* code = nullptr;
    ID3DBlob* msg = nullptr;
    const HRESULT hr = D3DCompile(kShader, strlen(kShader), "splat", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &msg);
    if (FAILED(hr)) error = msg ? std::string(static_cast<const char*>(msg->GetBufferPointer()), msg->GetBufferSize()) : "the splat shader did not compile";
    if (msg) msg->Release();
    return SUCCEEDED(hr) ? code : nullptr;
}

bool MakePipeline(ID3D11Device* dev) {
    if (g_vs && g_dev == dev) return true;
    g_dev = dev;
    ID3DBlob* vs = Compile("vs", "vs_5_0", g_error);
    ID3DBlob* ps = Compile("ps", "ps_5_0", g_error);
    if (!vs || !ps) return false;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps);
    vs->Release();
    ps->Release();
    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = 64;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&cb, nullptr, &g_cb);
    D3D11_BLEND_DESC bd = {};
    D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE; // (premultiplied)
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, &g_blend);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    dev->CreateRasterizerState(&rd, &g_raster);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dev->CreateDepthStencilState(&dd, &g_depth);
    return g_vs && g_ps && g_cb && g_blend && g_raster && g_depth;
}

bool MakeTarget(ID3D11Device* dev, int w, int h) {
    if (g_target && g_tw == w && g_th == h) return true;
    Drop(g_rtv), Drop(g_targetSrv), Drop(g_target);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w, td.Height = h, td.MipLevels = 1, td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_target))) return false;
    dev->CreateRenderTargetView(g_target, nullptr, &g_rtv);
    dev->CreateShaderResourceView(g_target, nullptr, &g_targetSrv);
    g_tw = w, g_th = h;
    return g_rtv && g_targetSrv;
}

void Upload(ID3D11Device* dev) {
    Drop(g_splatsSrv), Drop(g_orderSrv), Drop(g_splats), Drop(g_order);
    g_gpuCount = 0;
    if (g_cloud.empty()) return;
    const UINT n = static_cast<UINT>(g_cloud.size());
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = n * sizeof(Gpu);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(Gpu);
    D3D11_SUBRESOURCE_DATA init = {g_cloud.data(), 0, 0};
    if (FAILED(dev->CreateBuffer(&bd, &init, &g_splats))) {
        g_error = "The graphics card has no room for this many Gaussians";
        return;
    }
    bd.ByteWidth = n * sizeof(uint32_t);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.StructureByteStride = sizeof(uint32_t);
    dev->CreateBuffer(&bd, nullptr, &g_order);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.FirstElement = 0, sd.Buffer.NumElements = n;
    dev->CreateShaderResourceView(g_splats, &sd, &g_splatsSrv);
    if (g_order) dev->CreateShaderResourceView(g_order, &sd, &g_orderSrv);
    g_gpuCount = n;
    g_sorted.resize(n), g_keys.resize(n);
    g_sortNeeded = true;
}

// far ones first, by their distance along the view direction
void Sort(ID3D11DeviceContext* ctx, const float row2[4]) {
    const int n = g_gpuCount;
    float lo = 1e30f, hi = -1e30f;
    std::vector<float> z(n);
    for (int i = 0; i < n; ++i) {
        const float* p = g_cloud[i].pos;
        z[i] = row2[0] * p[0] + row2[1] * p[1] + row2[2] * p[2];
        lo = std::min(lo, z[i]), hi = std::max(hi, z[i]);
    }
    const float k = hi > lo ? 65535.0f / (hi - lo) : 0.0f;
    g_counts.assign(65537, 0);
    for (int i = 0; i < n; ++i) {
        g_keys[i] = static_cast<uint32_t>((hi - z[i]) * k); // (0 = the farthest)
        ++g_counts[g_keys[i] + 1];
    }
    for (int i = 1; i <= 65536; ++i) g_counts[i] += g_counts[i - 1];
    for (int i = 0; i < n; ++i) g_sorted[g_counts[g_keys[i]]++] = i;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_order, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, g_sorted.data(), n * sizeof(uint32_t));
        ctx->Unmap(g_order, 0);
    }
}

Cloud ReadPly(std::wstring path) {
    Cloud c;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        c.error = "Could not open the file";
        return c;
    }
    std::string line;
    if (!std::getline(in, line) || line.rfind("ply", 0) != 0) {
        c.error = "This is not a .ply file";
        return c;
    }
    size_t vertices = 0, stride = 0;
    bool binary = false, inVertex = false;
    std::map<std::string, size_t> at;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream s(line);
        std::string a, b, d;
        s >> a >> b >> d;
        if (a == "format") binary = b == "binary_little_endian";
        else if (a == "element") {
            inVertex = b == "vertex";
            if (inVertex) vertices = static_cast<size_t>(std::atoll(d.c_str()));
        } else if (a == "property" && inVertex) {
            if (b != "float" && b != "float32") {
                c.error = "Only float properties are read";
                return c;
            }
            at[d] = stride;
            stride += 4;
        } else if (a == "end_header") break;
    }
    for (const char* k : {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"})
        if (!binary || !at.count(k)) {
            c.error = "This .ply is not a 3D Gaussian splat (binary, with colours, opacity, scales and rotations)";
            return c;
        }
    if (vertices == 0 || vertices > 8000000) {
        c.error = "Nothing to show in this file";
        return c;
    }
    std::vector<char> raw(vertices * stride);
    in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    if (static_cast<size_t>(in.gcount()) != raw.size()) {
        c.error = "The file ends too soon";
        return c;
    }
    const size_t ox = at["x"], oy = at["y"], oz = at["z"], ofd = at["f_dc_0"], ofd1 = at["f_dc_1"], ofd2 = at["f_dc_2"], oop = at["opacity"], os0 = at["scale_0"],
                 os1 = at["scale_1"], os2 = at["scale_2"], or0 = at["rot_0"], or1 = at["rot_1"], or2 = at["rot_2"], or3 = at["rot_3"];
    auto get = [&](const char* v, size_t off) {
        float f;
        memcpy(&f, v + off, 4);
        return f;
    };
    c.splats.reserve(vertices);
    double sum[3] = {0, 0, 0}, weight = 0;
    for (size_t i = 0; i < vertices; ++i) {
        const char* v = raw.data() + i * stride;
        Gpu g = {};
        g.alpha = 1.0f / (1.0f + std::exp(-get(v, oop)));
        if (g.alpha < 0.004f) continue; // (nothing to see)
        g.pos[0] = get(v, ox), g.pos[1] = get(v, oy), g.pos[2] = get(v, oz);
        const float dc = 0.28209479f;
        g.color[0] = std::clamp(0.5f + dc * get(v, ofd), 0.0f, 1.0f);
        g.color[1] = std::clamp(0.5f + dc * get(v, ofd1), 0.0f, 1.0f);
        g.color[2] = std::clamp(0.5f + dc * get(v, ofd2), 0.0f, 1.0f);
        const float sx = std::exp(get(v, os0)), sy = std::exp(get(v, os1)), sz = std::exp(get(v, os2));
        float w = get(v, or0), x = get(v, or1), y = get(v, or2), z = get(v, or3);
        const float l = std::sqrt(w * w + x * x + y * y + z * z);
        if (l <= 0.0f) continue;
        w /= l, x /= l, y /= l, z /= l;
        const float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                               {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                               {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
        const float s2[3] = {sx * sx, sy * sy, sz * sz};
        auto cov = [&](int a, int b) { return r[a][0] * s2[0] * r[b][0] + r[a][1] * s2[1] * r[b][1] + r[a][2] * s2[2] * r[b][2]; };
        g.cxx = cov(0, 0), g.cxy = cov(0, 1), g.cxz = cov(0, 2), g.cyy = cov(1, 1), g.cyz = cov(1, 2), g.czz = cov(2, 2);
        for (int k = 0; k < 3; ++k) sum[k] += g.pos[k] * g.alpha;
        weight += g.alpha;
        c.splats.push_back(g);
    }
    if (c.splats.empty()) {
        c.error = "Nothing to show in this file";
        return c;
    }
    for (int k = 0; k < 3; ++k) c.center[k] = static_cast<float>(sum[k] / weight);
    std::vector<float> dist;
    dist.reserve(c.splats.size());
    for (const Gpu& g : c.splats) {
        const float d[3] = {g.pos[0] - c.center[0], g.pos[1] - c.center[1], g.pos[2] - c.center[2]};
        dist.push_back(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    }
    std::nth_element(dist.begin(), dist.begin() + dist.size() * 95 / 100, dist.end());
    c.radius = std::max(1e-4f, dist[dist.size() * 95 / 100]); // (stray ones do not push the camera back)
    // Idol says where the picture was taken from (<name>_camera.json, in its own z-up space): the view starts there, looking as the picture does
    std::wstring cam = path;
    cam.resize(cam.size() - 4); // (".ply")
    std::ifstream ci(cam + L"_camera.json", std::ios::binary);
    if (ci) {
        const nlohmann::json j = nlohmann::json::parse(ci, nullptr, false);
        if (j.is_object() && j.contains("yaw") && j.contains("pitch") && j["yaw"].is_number() && j["pitch"].is_number()) {
            c.yaw = 3.14159265f - j["yaw"].get<float>();
            c.pitch = std::clamp(j["pitch"].get<float>(), -1.5f, 1.5f);
        }
    }
    return c;
}

void Reset() {
    g_yaw = g_startYaw, g_pitch = g_startPitch, g_dist = g_radius * 4.4f;
    g_pan[0] = g_pan[1] = g_pan[2] = 0.0f;
    g_sortNeeded = true;
}

} // namespace

void Open(const std::wstring& ply) {
    if (g_reading.valid()) g_reading.wait();
    g_readingPath = ply;
    g_error.clear();
    g_reading = std::async(std::launch::async, ReadPly, ply);
}

void Close() {
    if (g_reading.valid()) g_reading.wait(), g_reading = {};
    g_cloud.clear(), g_cloud.shrink_to_fit();
    g_path.clear(), g_error.clear();
    g_upload = true;
}

bool Reading() { return g_reading.valid(); }
int Count() { return static_cast<int>(g_cloud.size()); }
const std::wstring& Path() { return g_path; }
const std::string& Error() { return g_error; }
bool Spinning() { return g_spin && !g_cloud.empty(); }

void Release() {
    if (g_reading.valid()) g_reading.wait(), g_reading = {};
    Drop(g_vs), Drop(g_ps), Drop(g_cb), Drop(g_blend), Drop(g_raster), Drop(g_depth);
    Drop(g_splatsSrv), Drop(g_orderSrv), Drop(g_splats), Drop(g_order);
    Drop(g_rtv), Drop(g_targetSrv), Drop(g_target);
}

void Draw(ID3D11Device* device, ImVec2 size) {
    if (g_reading.valid() && g_reading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Cloud c = g_reading.get();
        if (!c.error.empty()) {
            g_error = c.error;
        } else {
            g_cloud = std::move(c.splats);
            memcpy(g_center, c.center, sizeof(g_center));
            g_radius = c.radius;
            g_startYaw = c.yaw, g_startPitch = c.pitch;
            g_path = g_readingPath;
            g_upload = true;
            Reset();
        }
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const int w = std::max(16, static_cast<int>(size.x)), h = std::max(16, static_cast<int>(size.y));
    ImGui::InvisibleButton("##splatview", ImVec2(static_cast<float>(w), static_cast<float>(h)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hov = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(12, 14, 18, 255), ui::Dp(8.0f));
    if (!MakePipeline(device) || !MakeTarget(device, w, h)) {
        dl->AddText(ImVec2(p.x + 12, p.y + 12), IM_COL32(255, 120, 120, 255), g_error.empty() ? "The 3D view could not start" : g_error.c_str());
        return;
    }
    if (g_upload) g_upload = false, Upload(device);
    if (g_gpuCount == 0) return;
    // the camera
    const ImGuiIO& io = ImGui::GetIO();
    if (active && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_yaw -= io.MouseDelta.x * 0.006f;
            g_pitch = std::clamp(g_pitch + io.MouseDelta.y * 0.006f, -1.5f, 1.5f);
        }
    }
    if (hov && io.MouseWheel != 0.0f) g_dist = std::clamp(g_dist * std::pow(0.88f, io.MouseWheel), g_radius * 0.15f, g_radius * 30.0f);
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) Reset();
    if (g_spin && !active) g_yaw += io.DeltaTime * 0.5f;
    const float cy = std::cos(g_yaw), sy = std::sin(g_yaw), cp = std::cos(g_pitch), sp = std::sin(g_pitch);
    const float f[3] = {-cp * sy, -sp, -cp * cy};                    // forward (from the camera to what it looks at)
    const float r[3] = {cy, 0.0f, -sy};                              // right
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]}; // up
    if (active && (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle))) {
        const float k = g_dist * 0.0016f;
        for (int i = 0; i < 3; ++i) g_pan[i] += (-r[i] * io.MouseDelta.x + u[i] * io.MouseDelta.y) * k;
    }
    // model to world: the file's up is -y (the turn is about x), then around its centre
    const float s = g_flip ? -1.0f : 1.0f;
    const float target[3] = {g_pan[0], g_pan[1], g_pan[2]};
    const float eye[3] = {target[0] - f[0] * g_dist, target[1] - f[1] * g_dist, target[2] - f[2] * g_dist};
    auto row = [&](const float a[3], float out[4]) { // world row -> model row (x, y and z of the file), with the shift of the centre in w
        out[0] = a[0], out[1] = a[1] * s, out[2] = a[2] * s;
        out[3] = -(out[0] * g_center[0] + out[1] * g_center[1] + out[2] * g_center[2]) - (a[0] * eye[0] + a[1] * eye[1] + a[2] * eye[2]);
    };
    struct Cb {
        float r0[4], r1[4], r2[4];
        float focal[2], viewport[2];
    } cb;
    row(r, cb.r0), row(u, cb.r1), row(f, cb.r2);
    const float focal = 0.5f * h / std::tan(0.5f * 0.65f); // (37 degrees up and down)
    cb.focal[0] = cb.focal[1] = focal;
    cb.viewport[0] = static_cast<float>(w), cb.viewport[1] = static_cast<float>(h);
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx) return;
    float now[9];
    memcpy(now, f, sizeof(f)), memcpy(now + 3, cb.r2, 3 * sizeof(float)), now[6] = now[7] = now[8] = 0.0f;
    if (g_sortNeeded || memcmp(now, g_lastView, sizeof(now)) != 0) {
        memcpy(g_lastView, now, sizeof(now));
        g_sortNeeded = false;
        Sort(ctx, cb.r2);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &cb, sizeof(cb));
        ctx->Unmap(g_cb, 0);
    }
    const float clear[4] = {12 / 255.0f, 14 / 255.0f, 18 / 255.0f, 1.0f};
    ctx->ClearRenderTargetView(g_rtv, clear);
    // (what ImGui's own drawing sets up again for itself; the render target is put back by the main loop before it draws)
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_raster);
    ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    const float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(g_blend, bf, 0xffffffff);
    ctx->OMSetDepthStencilState(g_depth, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_cb);
    ID3D11ShaderResourceView* srvs[2] = {g_splatsSrv, g_orderSrv};
    ctx->VSSetShaderResources(0, 2, srvs);
    ctx->DrawInstanced(4, g_gpuCount, 0, 0);
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    ctx->VSSetShaderResources(0, 2, none);
    ID3D11RenderTargetView* noRt = nullptr;
    ctx->OMSetRenderTargets(1, &noRt, nullptr);
    ctx->Release();
    dl->AddImageRounded((ImTextureID)(intptr_t)g_targetSrv, p, ImVec2(p.x + w, p.y + h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, ui::Dp(8.0f));
}

} // namespace splat
