#pragma once
#include <d3d11.h>
#include <string>
#include <vector>

struct LoadedImage {
    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0;
    int height = 0;
};

// Decodes PNG/JPEG/BMP/GIF via stb_image (WebP etc. via Windows Imaging Component) and uploads it
// as an RGBA8 shader resource. Returns false and leaves `out` untouched on failure.
bool LoadImageTexture(ID3D11Device* device, const std::wstring& path, LoadedImage& out, std::string* error);

// Decodes an already-in-memory encoded image (e.g. bytes read back from a
// persisted thumbnail file) and uploads it as-is, with no resizing.
bool LoadImageTextureFromMemory(ID3D11Device* device, const unsigned char* data, size_t len, LoadedImage& out,
                                 std::string* error, std::vector<unsigned char>* outRawRGBA = nullptr);

// Same as LoadImageTexture, but downsamples (area-averaged) so neither
// dimension exceeds maxDim before uploading. Used for gallery thumbnails so
// memory use stays bounded no matter how large the source images or how many
// of them there are. Safe to call from a background thread: D3D11 device
// resource-creation methods are free-threaded as long as the device wasn't
// created with D3D11_CREATE_DEVICE_SINGLETHREADED (this app's device isn't).
// If outRawRGBA is non-null, it receives the final (downsized) RGBA pixels
// so the caller can persist them (e.g. to an on-disk thumbnail cache)
// without decoding the source image a second time.
bool LoadImageThumbnail(ID3D11Device* device, const std::wstring& path, int maxDim, LoadedImage& out,
                         std::vector<unsigned char>* outRawRGBA, std::string* error);

// Encodes an RGBA8 pixel buffer as PNG and writes it to `path`. Used to
// persist gallery thumbnails to the on-disk cache.
bool WriteThumbnailPng(const std::wstring& path, const unsigned char* rgba, int width, int height);

void ReleaseImage(LoadedImage& img); // (the texture goes once the frame is on screen: see ReleaseAfterFrame)

// Textures and views are let go only after the frame being drawn is presented (it may already draw them); any thread may call it.
void ReleaseAfterFrame(IUnknown* p);
void ReleaseFrameLeftovers(); // the UI thread, after Present (and before the device goes)

// The decoded pixels of a picture file (RGBA, 4 bytes a pixel), without making a texture.
bool DecodeImageRGBA(const std::wstring& path, std::vector<unsigned char>& rgba, int& width, int& height, std::string* error);
// A texture from RGBA pixels already in memory (pixel art, previews).
bool CreateImageFromRGBA(ID3D11Device* device, const unsigned char* rgba, int width, int height, LoadedImage& out, std::string* error);

// The NSFW blur: a soft, low-detail copy of a texture (made on the UI thread, a few a frame: null until it is made). Kept while it is drawn.
ID3D11ShaderResourceView* BlurredCopy(ID3D11Device* device, ID3D11ShaderResourceView* src);
void PruneBlurredCopies();  // once a frame: copies not drawn for a while are released
void ReleaseBlurredCopies(); // before the device goes
