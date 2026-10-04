#pragma once

#include <immintrin.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace pocket {

struct NnueNetwork {
    int featureDim = 0;
    int hidden = 0;
    int layers = 0;
    std::vector<float> ftWeight;
    std::vector<float> ftBias;
    std::vector<float> fc1w;
    std::vector<float> fc1b;
    std::vector<float> fc2w;
    std::vector<float> fc2b;
    std::vector<float> fc3w;
    std::vector<float> fc3b;
};

inline bool loadNnue(const std::string& path, NnueNetwork& net) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[4];
    f.read(magic, 4);
    if (std::string(magic, 4) != "PCNN") return false;
    uint32_t version;
    f.read(reinterpret_cast<char*>(&version), 4);
    if (version != 1) return false;
    uint32_t dim, hidden, layers;
    f.read(reinterpret_cast<char*>(&dim), 4);
    f.read(reinterpret_cast<char*>(&hidden), 4);
    f.read(reinterpret_cast<char*>(&layers), 4);
    if (hidden == 0 || hidden > 256) return false;  // inference uses a fixed 256-wide stack accumulator
    net.featureDim = dim;
    net.hidden = hidden;
    net.layers = layers;

    auto readF = [&](std::vector<float>& v, size_t n) {
        v.resize(n);
        f.read(reinterpret_cast<char*>(v.data()), n * sizeof(float));
    };

    // The file stores the feature transformer row-major by hidden unit
    // (stride = featureDim), which makes per-feature reads stride across the
    // whole array. Transpose once at load so each feature's `hidden` weights
    // are contiguous: inference reads one cache-friendly row per feature.
    {
        std::vector<float> fileFt;
        readF(fileFt, size_t(hidden) * dim);
        net.ftWeight.resize(size_t(dim) * hidden);
        for (uint32_t i = 0; i < hidden; ++i)
            for (uint32_t fd = 0; fd < dim; ++fd)
                net.ftWeight[size_t(fd) * hidden + i] = fileFt[size_t(i) * dim + fd];
    }
    readF(net.ftBias, hidden);
    readF(net.fc1w, 32 * hidden);
    readF(net.fc1b, 32);
    if (layers == 2) {
        readF(net.fc2w, 32 * 32);
        readF(net.fc2b, 32);
        readF(net.fc3w, 1 * 32);
        readF(net.fc3b, 1);
    } else {
        readF(net.fc2w, 1 * 32);
        readF(net.fc2b, 1);
    }
    return true;
}

inline float relu(float x) { return x > 0.0f ? x : 0.0f; }

inline float hsum8(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_add_ps(lo, _mm_movehl_ps(lo, lo));
    lo = _mm_add_ss(lo, _mm_shuffle_ps(lo, lo, 0x55));
    return _mm_cvtss_f32(lo);
}

inline unsigned long long g_ftNs = 0;
inline unsigned long long g_fcNs = 0;

inline float inference(const NnueNetwork& net, const std::vector<int>& features) {
    auto ta = std::chrono::steady_clock::now();
    float acc[256];
    for (int i = 0; i < net.hidden; ++i) acc[i] = net.ftBias[i];
    for (int f : features) {
        const float* w = &net.ftWeight[size_t(f) * net.hidden];
        for (int i = 0; i < net.hidden; ++i)
            acc[i] += w[i];
    }
    auto tb = std::chrono::steady_clock::now();
    // ReLU once, then dot products with 8-wide SIMD: a scalar `s += w*x`
    // reduction is a dependent add chain (256 x ~4 cycle latency per output),
    // which measured ~10us per call; the vector form keeps 8 partial lanes.
    float rx[256];
    for (int i = 0; i < net.hidden; ++i) rx[i] = relu(acc[i]);
    float h1[32];
    for (int o = 0; o < 32; ++o) {
        const float* w = &net.fc1w[size_t(o) * net.hidden];
        __m256 vsum = _mm256_setzero_ps();
        int i = 0;
        for (; i + 8 <= net.hidden; i += 8)
            vsum = _mm256_add_ps(vsum, _mm256_mul_ps(_mm256_loadu_ps(w + i),
                                                      _mm256_loadu_ps(rx + i)));
        float s = net.fc1b[o] + hsum8(vsum);
        for (; i < net.hidden; ++i) s += w[i] * rx[i];
        h1[o] = relu(s);
    }
    float out;
    if (net.layers == 2) {
        float h2[32];
        for (int o = 0; o < 32; ++o) {
            float s = net.fc2b[o];
            for (int i = 0; i < 32; ++i)
                s += net.fc2w[o * 32 + i] * h1[i];
            h2[o] = relu(s);
        }
        out = net.fc3b[0];
        for (int i = 0; i < 32; ++i)
            out += net.fc3w[i] * h2[i];
    } else {
        out = net.fc2b[0];
        for (int i = 0; i < 32; ++i)
            out += net.fc2w[i] * h1[i];
    }
    auto tc = std::chrono::steady_clock::now();
    g_ftNs += std::chrono::duration_cast<std::chrono::nanoseconds>(tb - ta).count();
    g_fcNs += std::chrono::duration_cast<std::chrono::nanoseconds>(tc - tb).count();
    return out;
}

}  // namespace pocket
