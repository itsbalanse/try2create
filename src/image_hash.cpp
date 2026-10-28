#include "image_hash.h"

#include <algorithm>
#include <intrin.h>
#include <numeric>
#include <unordered_map>

uint64_t DHashFromRGBA(const unsigned char* rgba, int w, int h) {
    if (!rgba || w < 2 || h < 2) return 0;
    // 9x8 grayscale grid by area averaging.
    double gray[8][9];
    for (int gy = 0; gy < 8; ++gy) {
        int y0 = static_cast<int>(static_cast<long long>(gy) * h / 8);
        int y1 = std::max(y0 + 1, static_cast<int>(static_cast<long long>(gy + 1) * h / 8));
        for (int gx = 0; gx < 9; ++gx) {
            int x0 = static_cast<int>(static_cast<long long>(gx) * w / 9);
            int x1 = std::max(x0 + 1, static_cast<int>(static_cast<long long>(gx + 1) * w / 9));
            double sum = 0;
            for (int y = y0; y < y1; ++y) {
                const unsigned char* p = rgba + (static_cast<size_t>(y) * w + x0) * 4;
                for (int x = x0; x < x1; ++x, p += 4) sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
            }
            gray[gy][gx] = sum / (static_cast<double>(y1 - y0) * (x1 - x0));
        }
    }
    uint64_t hash = 0;
    for (int gy = 0; gy < 8; ++gy) {
        for (int gx = 0; gx < 8; ++gx) {
            hash <<= 1;
            if (gray[gy][gx] < gray[gy][gx + 1]) hash |= 1;
        }
    }
    return hash;
}

int HammingDistance(uint64_t a, uint64_t b) {
    return static_cast<int>(__popcnt64(a ^ b));
}

namespace {

struct DisjointSet {
    std::vector<int> parent;
    explicit DisjointSet(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int Find(int x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void Union(int a, int b) { parent[Find(a)] = Find(b); }
};

constexpr size_t kMaxBucket = 4000; // pathological buckets (e.g. thousands of flat images) are skipped

} // namespace

std::vector<std::vector<int>> GroupSimilar(const std::vector<HashedItem>& items, int maxDistance,
                                           const std::atomic<bool>* cancel) {
    std::vector<std::vector<int>> groups;
    maxDistance = std::clamp(maxDistance, 0, 16);

    // Identical hashes collapse to one node; members are restored at the end.
    std::unordered_map<uint64_t, int> nodeOfHash;
    std::vector<uint64_t> nodeHash;
    std::vector<std::vector<int>> nodeMembers;
    for (const HashedItem& item : items) {
        auto [it, inserted] = nodeOfHash.try_emplace(item.hash, static_cast<int>(nodeHash.size()));
        if (inserted) {
            nodeHash.push_back(item.hash);
            nodeMembers.emplace_back();
        }
        nodeMembers[it->second].push_back(item.index);
    }

    DisjointSet sets(nodeHash.size());
    if (maxDistance > 0) {
        // Two hashes within d bits must agree exactly on at least one of d+1 chunks.
        const int chunks = maxDistance + 1;
        for (int c = 0; c < chunks; ++c) {
            if (cancel && cancel->load()) return {};
            const int lo = c * 64 / chunks, hi = (c + 1) * 64 / chunks;
            const int bits = hi - lo;
            const uint64_t mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
            std::unordered_map<uint64_t, std::vector<int>> buckets;
            for (size_t n = 0; n < nodeHash.size(); ++n) {
                buckets[(nodeHash[n] >> lo) & mask].push_back(static_cast<int>(n));
            }
            for (auto& [key, bucket] : buckets) {
                if (bucket.size() < 2 || bucket.size() > kMaxBucket) continue;
                for (size_t i = 0; i < bucket.size(); ++i) {
                    for (size_t j = i + 1; j < bucket.size(); ++j) {
                        if (HammingDistance(nodeHash[bucket[i]], nodeHash[bucket[j]]) <= maxDistance) {
                            sets.Union(bucket[i], bucket[j]);
                        }
                    }
                }
                if (cancel && cancel->load()) return {};
            }
        }
    }

    std::unordered_map<int, std::vector<int>> byRoot;
    for (size_t n = 0; n < nodeHash.size(); ++n) {
        auto& dst = byRoot[sets.Find(static_cast<int>(n))];
        dst.insert(dst.end(), nodeMembers[n].begin(), nodeMembers[n].end());
    }
    for (auto& [root, members] : byRoot) {
        if (members.size() >= 2) {
            std::sort(members.begin(), members.end());
            groups.push_back(std::move(members));
        }
    }
    std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) {
        return a.size() != b.size() ? a.size() > b.size() : a[0] < b[0];
    });
    return groups;
}
