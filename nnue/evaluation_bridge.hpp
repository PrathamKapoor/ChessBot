// Bridge between the search and the NNUE evaluator.
// When enabled, evaluate() returns NNUE output instead of classical eval.
#pragma once

#include <chrono>

#include "nnue_inference.hpp"
#include "nnue_features.hpp"
#include "../engine/evaluate.hpp"
#include "../engine/position.hpp"

namespace pocket {

inline NnueNetwork g_nnueNet;
inline bool g_useNnue = false;
inline unsigned long long g_nnueEvalCalls = 0;
inline unsigned long long g_nnueEvalNs = 0;
inline unsigned long long g_nnueFeatNs = 0;
inline unsigned long long g_nnueInferNs = 0;

inline bool loadGlobalNnue(const std::string& path) {
    return loadNnue(path, g_nnueNet);
}

inline int evalNnue(const Position& pos) {
    auto t0 = std::chrono::steady_clock::now();
    auto feats = activeFeatures(pos);
    auto t1 = std::chrono::steady_clock::now();
    float out = inference(g_nnueNet, feats);
    auto t2 = std::chrono::steady_clock::now();
    g_nnueEvalCalls++;
    g_nnueEvalNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t0).count();
    g_nnueFeatNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    g_nnueInferNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
    // Convert float output to centipawns (1.0 = ~400cp since model trained on cp targets)
    return static_cast<int>(out * 400.0f);
}

inline int evalPosition(const Position& pos) {
    if (g_useNnue) return evalNnue(pos);
    return evaluate(pos);
}

}  // namespace pocket
