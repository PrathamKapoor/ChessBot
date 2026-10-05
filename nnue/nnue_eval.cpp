// C++ NNUE inference test: load network, evaluate a position, print result.
#include <cstdio>
#include <cstring>
#include <string>

#include "../engine/position.hpp"
#include "nnue_features.hpp"
#include "nnue_inference.hpp"

using namespace pocket;

int main(int argc, char** argv) {
    std::string fen, path;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fen") && i + 1 < argc) fen = argv[++i];
        else if (!std::strcmp(argv[i], "--net") && i + 1 < argc) path = argv[++i];
    }
    if (fen.empty() || path.empty()) {
        std::printf("usage: nnue_eval --fen FEN --net PATH\n");
        return 2;
    }
    NnueNetwork net;
    if (!loadNnue(path, net)) {
        std::printf("failed to load network\n");
        return 2;
    }
    Position pos;
    std::string err;
    if (!pos.setFromFen(fen, &err)) {
        std::printf("FEN error: %s\n", err.c_str());
        return 2;
    }
    auto feats = activeFeatures(pos);
    float out = inference(net, feats);
    // Raw network output (score/400); golden_test.py applies the x400 the
    // evaluation bridge uses. Printed with enough digits that the text form is
    // not the limiting factor in the parity comparison.
    std::printf("%.9g\n", out);
    return 0;
}
