// Print active feature indices for a position, for cross-checking with Python.
#include <cstdio>
#include <cstring>
#include <string>

#include "../engine/position.hpp"
#include "nnue_features.hpp"

using namespace pocket;

int main(int argc, char** argv) {
    std::string fen;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fen") && i + 1 < argc) fen = argv[++i];
    }
    if (fen.empty()) {
        std::printf("usage: feature_check --fen FEN\n");
        return 2;
    }
    Position pos;
    std::string err;
    if (!pos.setFromFen(fen, &err)) {
        std::printf("FEN error: %s\n", err.c_str());
        return 2;
    }
    auto feats = activeFeatures(pos);
    for (int f : feats) std::printf("%d\n", f);
    return 0;
}
