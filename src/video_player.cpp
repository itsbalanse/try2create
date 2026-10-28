#include "video_player.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfmediaengine.h>
#include <d3d11_4.h>
#include <wincodec.h>

#include "image_texture.h"
#include "video_util.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::wstring ExtOf(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return {};
    std::wstring e = path.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), ::towlower);
    return e;
}

// The texture ImGui draws; recreated when the frame size changes.
struct FrameTexture {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
    bool dynamic = false;
    void Release() { // (once the frame is on screen: a clip switched mid-frame was still in its draw list, see ReleaseAfterFrame)
        ReleaseAfterFrame(srv);
        ReleaseAfterFrame(tex);
        srv = nullptr;
        tex = nullptr;
        w = h = 0;
    }
    bool Ensure(ID3D11Device* dev, int width, int height, bool wantDynamic) {
        if (tex && w == width && h == height && dynamic == wantDynamic) return true;
        Release();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(width);
        d.Height = static_cast<UINT>(height);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        if (wantDynamic) {
            d.Usage = D3D11_USAGE_DYNAMIC;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        } else {
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        }
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex))) return false;
        if (FAILED(dev->CreateShaderResourceView(tex, nullptr, &srv))) {
            Release();
            return false;
        }
        w = width;
        h = height;
        dynamic = wantDynamic;
        return true;
    }
};

} // namespace

struct VideoPlayer::Backend {
    virtual ~Backend() = default;
    virtual bool Start(const std::wstring& path, float volume, bool muted, bool loop) = 0;
    // true when the picture changed; `failed` is set when playback cannot go on
    virtual bool Update(bool& failed) = 0;
    virtual bool Playing() const = 0;
    virtual void Play() = 0;
    virtual void Pause() = 0;
    virtual void Seek(double seconds) = 0;
    virtual double Position() const = 0;
    virtual double Duration() const = 0;
    virtual void SetVolume(float) {}
    virtual void SetMuted(bool) {}
    virtual void SetLoop(bool) = 0;
    virtual bool HasSound() const { return false; }
    virtual const char* Name() const = 0;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    FrameTexture frame;
};

namespace {

// ---------------------------------------------------------------------------------------------
// Media Foundation Media Engine: CPU decoding, sound, seeking
// ---------------------------------------------------------------------------------------------
struct NotifyState {
    std::atomic<bool> loaded{false}, error{false}, ended{false};
};

class MfNotify : public IMFMediaEngineNotify {
public:
    explicit MfNotify(std::shared_ptr<NotifyState> s) : state_(std::move(s)) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IMFMediaEngineNotify) || riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR, DWORD) override {
        switch (event) {
            case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
            case MF_MEDIA_ENGINE_EVENT_CANPLAY: state_->loaded = true; break;
            case MF_MEDIA_ENGINE_EVENT_ERROR: state_->error = true; break;
            case MF_MEDIA_ENGINE_EVENT_ENDED: state_->ended = true; break;
            case MF_MEDIA_ENGINE_EVENT_PLAYING: state_->ended = false; break;
            default: break;
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<NotifyState> state_;
};

class MfBackend : public VideoPlayer::Backend {
public:
    ~MfBackend() override {
        if (eng_) {
            eng_->Pause();
            eng_->Shutdown();
        }
        SafeRelease(eng_);
        SafeRelease(bmp_);
        SafeRelease(wic_);
        frame.Release();
    }

    bool Start(const std::wstring& path, float volume, bool muted, bool loop) override {
        static std::once_flag once;
        static bool mfOk = false;
        std::call_once(once, []() { mfOk = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL)); });
        if (!mfOk) return false;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_)))) return false;

        // Decoded on the CPU, into a bitmap of our own: the engine gets no D3D device. With the app's device (DXVA), the engine's threads
        // built and tore down decoder surfaces on it while the window presented; under a GPU kept at 100% by ComfyUI that crashed the NVIDIA
        // driver (nvwgf2umx, the same fault 4 times, always while a clip opened or switched) and the first picture stalled behind the generation.
        // A 1280x1024 H.264 clip costs a few percent of one core.
        state_ = std::make_shared<NotifyState>();
        MfNotify* notify = new MfNotify(state_);
        IMFAttributes* attrs = nullptr;
        MFCreateAttributes(&attrs, 2);
        attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
        attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
        IMFMediaEngineClassFactory* factory = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateInstance(0, attrs, &eng_);
        SafeRelease(factory);
        SafeRelease(attrs);
        notify->Release();
        if (FAILED(hr) || !eng_) return false;

        eng_->SetAutoPlay(TRUE);
        eng_->SetVolume(volume);
        eng_->SetMuted(muted);
        eng_->SetLoop(loop);
        BSTR src = SysAllocString(path.c_str());
        hr = eng_->SetSource(src);
        SysFreeString(src);
        return SUCCEEDED(hr);
    }

    bool Update(bool& failed) override {
        failed = state_->error.load();
        if (failed || !eng_) return false;
        bool changed = false;
        if (state_->loaded.load() && eng_->HasVideo()) {
            LONGLONG pts = 0;
            if (eng_->OnVideoStreamTick(&pts) == S_OK) {
                DWORD w = 0, h = 0;
                eng_->GetNativeVideoSize(&w, &h);
                if (w > 0 && h > 0 && (bw_ != w || bh_ != h)) {
                    SafeRelease(bmp_);
                    bw_ = bh_ = 0;
                    if (SUCCEEDED(wic_->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &bmp_))) bw_ = w, bh_ = h;
                }
                if (bmp_ && frame.Ensure(dev, static_cast<int>(w), static_cast<int>(h), true)) {
                    const MFVideoNormalizedRect whole{0, 0, 1, 1};
                    RECT dst{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
                    MFARGB border{0, 0, 0, 255};
                    if (SUCCEEDED(eng_->TransferVideoFrame(bmp_, &whole, &dst, &border)) && Upload()) {
                        changed = true;
                        misses_ = 0;
                    } else if (++misses_ > 30 && !shown_) {
                        failed = true; // no picture at all: the player falls back to ffmpeg
                    }
                }
            }
        }
        return changed || Playing();
    }

    // the bitmap into the texture ImGui draws (on the UI thread, like every other use of the device)
    bool Upload() {
        WICRect all{0, 0, static_cast<INT>(bw_), static_cast<INT>(bh_)};
        IWICBitmapLock* lock = nullptr;
        if (FAILED(bmp_->Lock(&all, WICBitmapLockRead, &lock))) return false;
        UINT stride = 0, size = 0;
        BYTE* data = nullptr;
        bool ok = false;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(lock->GetStride(&stride)) && SUCCEEDED(lock->GetDataPointer(&size, &data)) && data && stride >= bw_ * 4 &&
            SUCCEEDED(ctx->Map(frame.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            for (UINT y = 0; y < bh_; ++y) {
                BYTE* row = static_cast<BYTE*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                memcpy(row, data + static_cast<size_t>(y) * stride, bw_ * 4);
                for (UINT x = 0; x < bw_; ++x) row[x * 4 + 3] = 255; // video is opaque
            }
            ctx->Unmap(frame.tex, 0);
            ok = shown_ = true;
        }
        lock->Release();
        return ok;
    }

    bool Playing() const override { return eng_ && !eng_->IsPaused() && !eng_->IsEnded(); }
    void Play() override {
        if (!eng_) return;
        if (eng_->IsEnded()) eng_->SetCurrentTime(0);
        eng_->Play();
    }
    void Pause() override {
        if (eng_) eng_->Pause();
    }
    void Seek(double s) override {
        if (eng_) eng_->SetCurrentTime(s);
    }
    double Position() const override { return eng_ ? eng_->GetCurrentTime() : 0.0; }
    double Duration() const override {
        const double d = eng_ ? eng_->GetDuration() : 0.0;
        return std::isfinite(d) ? d : 0.0;
    }
    void SetVolume(float v) override {
        if (eng_) eng_->SetVolume(v);
    }
    void SetMuted(bool m) override {
        if (eng_) eng_->SetMuted(m);
    }
    void SetLoop(bool l) override {
        if (eng_) eng_->SetLoop(l);
    }
    bool HasSound() const override { return eng_ && eng_->HasAudio(); }
    const char* Name() const override { return "Windows"; }

private:
    IMFMediaEngine* eng_ = nullptr;
    IWICImagingFactory* wic_ = nullptr;
    IWICBitmap* bmp_ = nullptr;
    UINT bw_ = 0, bh_ = 0;
    int misses_ = 0;
    bool shown_ = false;
    std::shared_ptr<NotifyState> state_;
};

// ---------------------------------------------------------------------------------------------
// ffmpeg: raw frames over a pipe, paced to the clip's frame rate (WebM, MKV, other codecs); no sound
// ---------------------------------------------------------------------------------------------
class FfBackend : public VideoPlayer::Backend {
public:
    ~FfBackend() override {
        StopProcess();
        frame.Release();
    }

    bool Start(const std::wstring& path, float, bool, bool loop) override {
        if (FfmpegPath().empty()) return false;
        path_ = path;
        loop_ = loop;
        VideoInfo info = ProbeVideoFfmpeg(path);
        if (info.width <= 0 || info.height <= 0) return false;
        duration_ = info.seconds;
        fps_ = info.fps > 1.0 && info.fps < 240.0 ? info.fps : 24.0;
        const double scale = std::min(1.0, 1280.0 / std::max(info.width, info.height));
        outW_ = std::max(2, static_cast<int>(info.width * scale) & ~1);
        outH_ = std::max(2, static_cast<int>(info.height * scale) & ~1);
        paused_ = false;
        return Launch(0.0);
    }

    bool Update(bool& failed) override {
        failed = failed_.load();
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (hasNew_ && frame.Ensure(dev, outW_, outH_, true)) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx->Map(frame.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    const size_t rowBytes = static_cast<size_t>(outW_) * 4;
                    for (int y = 0; y < outH_; ++y) {
                        std::memcpy(static_cast<unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, latest_.data() + y * rowBytes, rowBytes);
                    }
                    ctx->Unmap(frame.tex, 0);
                    changed = true;
                }
                hasNew_ = false;
            }
        }
        if (ended_.load() && loop_) {
            Seek(0.0);
            paused_ = false;
        }
        return changed || Playing();
    }

    bool Playing() const override { return !paused_.load() && !ended_.load(); }
    void Play() override {
        if (ended_.load()) Seek(0.0);
        paused_ = false;
    }
    void Pause() override { paused_ = true; }
    void Seek(double s) override {
        s = std::clamp(s, 0.0, duration_ > 0 ? std::max(0.0, duration_ - 0.05) : s);
        const bool wasPaused = paused_.load();
        StopProcess();
        ended_ = false;
        paused_ = wasPaused;
        Launch(s);
    }
    double Position() const override { return base_ + static_cast<double>(frames_.load()) / fps_; }
    double Duration() const override { return duration_; }
    void SetLoop(bool l) override { loop_ = l; }
    const char* Name() const override { return "ffmpeg"; }

private:
    bool Launch(double startSeconds) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 1 << 22)) return false;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        char ss[32];
        snprintf(ss, sizeof(ss), "%.3f", startSeconds);
        std::wstring cmd = L"\"" + FfmpegPath() + L"\" -v error -ss " + std::wstring(ss, ss + std::strlen(ss)) + L" -i \"" + path_ +
                           L"\" -an -vf scale=" + std::to_wstring(outW_) + L":" + std::to_wstring(outH_) +
                           L" -f rawvideo -pix_fmt bgra -";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = nul;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(wr);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) {
            CloseHandle(rd);
            return false;
        }
        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;
        base_ = startSeconds;
        frames_ = 0;
        stop_ = false;
        thread_ = std::thread([this, rd]() { ReadLoop(rd); });
        return true;
    }

    void StopProcess() {
        stop_ = true;
        if (proc_) TerminateProcess(proc_, 0);
        if (thread_.joinable()) thread_.join();
        if (proc_) {
            CloseHandle(proc_);
            proc_ = nullptr;
        }
    }

    void ReadLoop(HANDLE rd) {
        using clock = std::chrono::steady_clock;
        const size_t frameBytes = static_cast<size_t>(outW_) * outH_ * 4;
        std::vector<unsigned char> buf(frameBytes);
        auto t0 = clock::now();
        long long idx = 0;
        while (!stop_.load()) {
            size_t got = 0;
            bool eof = false;
            while (got < frameBytes) {
                DWORD n = 0;
                if (!ReadFile(rd, buf.data() + got, static_cast<DWORD>(frameBytes - got), &n, nullptr) || n == 0) {
                    eof = true;
                    break;
                }
                got += n;
            }
            if (eof || stop_.load()) {
                if (!stop_.load()) {
                    if (idx == 0) failed_ = true; // ffmpeg produced nothing
                    else ended_ = true;
                }
                break;
            }
            // pace to the frame rate; the first frame after a start/seek is shown even when paused
            if (idx > 0) {
                auto pauseStart = clock::now();
                bool wasPaused = false;
                while (!stop_.load()) {
                    if (paused_.load()) {
                        wasPaused = true;
                        std::this_thread::sleep_for(std::chrono::milliseconds(15));
                        continue;
                    }
                    if (wasPaused) {
                        t0 += clock::now() - pauseStart;
                        wasPaused = false;
                    }
                    const auto target = t0 + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(static_cast<double>(idx) / fps_));
                    const auto now = clock::now();
                    if (now >= target) break;
                    std::this_thread::sleep_for(std::min<clock::duration>(target - now, std::chrono::milliseconds(8)));
                }
                if (stop_.load()) break;
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                latest_.assign(buf.begin(), buf.end());
                hasNew_ = true;
            }
            ++idx;
            frames_ = idx;
        }
        CloseHandle(rd);
    }

    std::wstring path_;
    HANDLE proc_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false}, paused_{false}, ended_{false}, failed_{false};
    std::mutex mu_;
    std::vector<unsigned char> latest_;
    bool hasNew_ = false;
    std::atomic<long long> frames_{0};
    double base_ = 0, fps_ = 24, duration_ = 0;
    int outW_ = 0, outH_ = 0;
    bool loop_ = false;
};

} // namespace

// ---------------------------------------------------------------------------------------------
namespace {
std::vector<VideoPlayer*> g_players; // all of them, for LetGo / TakeBack (UI thread only)
float g_videoVolume = 0.8f;
bool IsSoundFile(const std::wstring& path) {
    std::wstring ext = path.substr(path.find_last_of(L'.') == std::wstring::npos ? path.size() : path.find_last_of(L'.'));
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext == L".wav" || ext == L".mp3" || ext == L".flac" || ext == L".ogg" || ext == L".opus" || ext == L".m4a" || ext == L".aac";
}
} // namespace

void VideoPlayer::SetVideoVolume(float v) {
    g_videoVolume = std::clamp(v, 0.0f, 1.0f);
    for (VideoPlayer* p : g_players) {
        if (p->followVolume_ && p->open_ && !IsSoundFile(p->path_)) p->SetVolume(g_videoVolume);
    }
}

float VideoPlayer::VideoVolume() { return g_videoVolume; }

VideoPlayer::VideoPlayer() { g_players.push_back(this); }
VideoPlayer::~VideoPlayer() {
    Close();
    g_players.erase(std::remove(g_players.begin(), g_players.end(), this), g_players.end());
}

void VideoPlayer::LetGo(const std::wstring& path) {
    for (VideoPlayer* p : g_players) {
        if (!p->open_ || _wcsicmp(p->path_.c_str(), path.c_str()) != 0) continue;
        p->resumeAt_ = p->Position();
        p->resumePlaying_ = p->Playing();
        p->Close();
        p->letGo_ = true;
    }
}

void VideoPlayer::TakeBack(const std::wstring& path) {
    for (VideoPlayer* p : g_players) {
        if (!p->letGo_ || _wcsicmp(p->path_.c_str(), path.c_str()) != 0) continue;
        p->letGo_ = false;
        const double at = p->resumeAt_;
        const bool playing = p->resumePlaying_;
        if (!p->Open(p->device_, p->context_, p->path_, p->volume_, p->muted_, p->loop_)) continue;
        p->resumeAt_ = at;
        p->resumePlaying_ = playing;
        if (!playing) p->Pause();
    }
}

bool VideoPlayer::StartBackend(bool ffmpeg) {
    backend_.reset();
    std::unique_ptr<Backend> b;
    if (ffmpeg) b = std::make_unique<FfBackend>();
    else b = std::make_unique<MfBackend>();
    b->dev = device_;
    b->ctx = context_;
    if (!b->Start(path_, volume_, muted_, loop_)) return false;
    backend_ = std::move(b);
    return true;
}

bool VideoPlayer::Open(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& path, float volume, bool muted, bool loop, bool autoplay) {
    Close();
    device_ = device;
    context_ = context;
    path_ = path;
    volume_ = followVolume_ && !IsSoundFile(path) ? g_videoVolume : volume; // (a video: the app's video volume)
    startPaused_ = !autoplay;
    wantMuted_ = muted;
    muted_ = muted || !autoplay; // (silent for the moment it plays to get its first frame)
    loop_ = loop;
    error_.clear();
    gotFrame_ = false;
    triedFfmpeg_ = false;
    letGo_ = false;
    resumeAt_ = -1;
    const std::wstring ext = ExtOf(path);
    const bool haveFfmpeg = !FfmpegPath().empty();
    bool started = false;
    if ((ext == L".webm" || ext == L".mkv") && haveFfmpeg) {
        started = StartBackend(true);
        triedFfmpeg_ = true;
    }
    if (!started) started = StartBackend(false);
    if (!started && haveFfmpeg && !triedFfmpeg_) {
        started = StartBackend(true);
        triedFfmpeg_ = true;
    }
    if (!started) {
        error_ = haveFfmpeg ? "This video can't be played." : "Windows can't play this video. Install ffmpeg (on the PATH) for WebM, MKV and other formats.";
        return false;
    }
    open_ = true;
    return true;
}

void VideoPlayer::Close() {
    backend_.reset();
    open_ = false;
    gotFrame_ = false;
}

bool VideoPlayer::Update() {
    if (!open_ || !backend_) return false;
    bool failed = false;
    const bool changed = backend_->Update(failed);
    if (startPaused_ && backend_->frame.srv) { // opened without autoplay: its first frame is shown, now it waits
        startPaused_ = false;
        backend_->Pause();
        backend_->Seek(0);
        muted_ = wantMuted_;
        backend_->SetMuted(muted_);
    }
    if (resumeAt_ >= 0 && backend_->Duration() > 0) { // reopened by TakeBack: back where it was
        backend_->Seek(std::min(resumeAt_, backend_->Duration()));
        if (!resumePlaying_) backend_->Pause();
        resumeAt_ = -1;
    }
    if (backend_->frame.srv) gotFrame_ = true;
    if (failed) {
        if (!gotFrame_ && !triedFfmpeg_ && !FfmpegPath().empty()) {
            triedFfmpeg_ = true;
            if (!StartBackend(true)) error_ = "This video can't be played.";
        } else {
            error_ = gotFrame_ ? "Playback stopped unexpectedly." : "This video can't be played (its codec isn't available).";
            backend_.reset();
            return false;
        }
    }
    return changed;
}

ID3D11ShaderResourceView* VideoPlayer::Srv() const { return backend_ ? backend_->frame.srv : nullptr; }
int VideoPlayer::Width() const { return backend_ ? backend_->frame.w : 0; }
int VideoPlayer::Height() const { return backend_ ? backend_->frame.h : 0; }
bool VideoPlayer::Playing() const { return backend_ && backend_->Playing(); }
bool VideoPlayer::HasSound() const { return backend_ && backend_->HasSound(); }
const char* VideoPlayer::BackendName() const { return backend_ ? backend_->Name() : ""; }
void VideoPlayer::Play() { if (backend_) backend_->Play(); }
void VideoPlayer::Pause() { if (backend_) backend_->Pause(); }
void VideoPlayer::Seek(double s) { if (backend_) backend_->Seek(s); }
double VideoPlayer::Position() const { return backend_ ? backend_->Position() : 0.0; }
double VideoPlayer::Duration() const { return backend_ ? backend_->Duration() : 0.0; }
void VideoPlayer::SetVolume(float v) {
    volume_ = v;
    if (backend_) backend_->SetVolume(v);
}
void VideoPlayer::SetMuted(bool m) {
    muted_ = m;
    if (backend_) backend_->SetMuted(m);
}
void VideoPlayer::SetLoop(bool l) {
    loop_ = l;
    if (backend_) backend_->SetLoop(l);
}
