#pragma once
// Perceptual image hashing for finding near-duplicate images.
#include <atomic>
#include <cstdint>
#include <vector>

// 64-bit difference hash of an RGBA8 image: robust to rescaling, re-encoding
// and small colour changes, so resized/recompressed copies land a few bits apart.
uint64_t DHashFromRGBA(const unsigned char* rgba, int width, int height);

int HammingDistance(uint64_t a, uint64_t b);

struct HashedItem {
    int index = 0;     // caller's identifier (index into the gallery)
    uint64_t hash = 0;
};

// Groups items whose hashes differ by at most `maxDistance` bits (transitively).
// Only groups of two or more are returned, largest first. Uses multi-index
// hashing (pigeonhole over hash chunks) so large folders stay fast. Returns an
// empty list early if `cancel` becomes true.
std::vector<std::vector<int>> GroupSimilar(const std::vector<HashedItem>& items, int maxDistance,
                                           const std::atomic<bool>* cancel);
