#pragma once
// Plays a video inside the app: frames land in a D3D11 texture that ImGui draws like any image.
// MP4/MOV/M4V/AVI use the Windows Media Foundation Media Engine (with sound); WebM/MKV and codecs Windows
// lacks use ffmpeg.exe when it is on the PATH (picture only).
#include <d3d11.h>
#include <memory>
#include <string>

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // Starts playing `path` (autoplay false: shows its first frame and waits, paused, silent until then). Returns false (with Error())
    // when nothing can play it.
    bool Open(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& path, float volume, bool muted, bool loop, bool autoplay = true);
    void Close();
    bool IsOpen() const { return open_; }
    const std::wstring& Path() const { return path_; }

    // Call once per UI frame. Returns true while the picture is changing (keep redrawing).
    bool Update();

    ID3D11ShaderResourceView* Srv() const;
    int Width() const;
    int Height() const;
    bool Playing() const;
    bool Failed() const { return !error_.empty(); }
    const std::string& Error() const { return error_; }
    bool HasSound() const;
    const char* BackendName() const;

    void Play();
    void Pause();
    void Toggle() { Playing() ? Pause() : Play(); }
    void Seek(double seconds);
    double Position() const;
    double Duration() const;
    void SetVolume(float volume);
    void SetMuted(bool muted);
    void SetLoop(bool loop);

    // Every player (UI thread only) that has `path` open lets go of the file, so it can be replaced (a repaired video), and TakeBack
    // reopens it where it was, playing or paused.
    static void LetGo(const std::wstring& path);
    static void TakeBack(const std::wstring& path);
    // The volume every video plays at, wherever it plays (0..1; Settings, View menu, any video's volume slider). It applies to the open
    // ones too. Sound files (a song, a drama take) and a player that opted out (the music player) keep their own volume.
    static void SetVideoVolume(float volume);
    static float VideoVolume();
    void FollowVideoVolume(bool on) { followVolume_ = on; }

    struct Backend; // implementation detail

private:
    std::unique_ptr<Backend> backend_;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    std::wstring path_;
    std::string error_;
    float volume_ = 1.0f;
    bool muted_ = false, loop_ = false;
    bool open_ = false;
    bool followVolume_ = true;
    bool triedFfmpeg_ = false;
    bool gotFrame_ = false;
    bool letGo_ = false, resumePlaying_ = false;
    bool startPaused_ = false, wantMuted_ = false; // opened without autoplay: paused (and its sound back) once the first frame is in
    double resumeAt_ = -1; // seek here once the reopened file knows its length
    bool StartBackend(bool ffmpeg);
};
