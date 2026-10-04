// NNUE feature extraction for Pocket Chess.
// Must match nnue/features.py exactly.
#pragma once

#include <vector>

#include "../engine/position.hpp"
#include "../engine/types.hpp"

namespace pocket {

inline int pieceIndex(Piece pc) {
  if (int(pc) >= kWhitePawn && int(pc) <= kWhiteKing)
    return int(pc) - int(kWhitePawn);
  return int(pc) - int(kBlackPawn) + 6;
}

inline int sqFile(Square s) { return int(s) % 8; }
inline int sqRank(Square s) { return int(s) / 8; }
inline Square makeSq(int f, int r) { return Square(f + r * 8); }

inline Square mirrorSq(Square s) {
  int f = sqFile(s), r = sqRank(s);
  return makeSq(7 - f, r);
}

inline Square mirrorKingSq(Square kingSq) {
  int f = sqFile(kingSq), r = sqRank(kingSq);
  if (f >= 4) f = 7 - f;
  return makeSq(f, r);
}

inline std::vector<int> activeFeatures(const Position& pos) {
  std::vector<int> out;
  Color side = pos.sideToMove();
  Square kingSq = kSQNone;
  for (int s = 0; s < 64; ++s) {
    Piece pc = pos.pieceOn(Square(s));
    if (pc == makePiece(side, kKing)) {
      kingSq = Square(s);
      break;
    }
  }
  if (kingSq == kSQNone) return out;

  bool mirror = sqFile(kingSq) >= 4;
  if (mirror) kingSq = mirrorKingSq(kingSq);

  for (int s = 0; s < 64; ++s) {
    Square sq = Square(s);
    Piece pc = pos.pieceOn(sq);
    if (pc == kNoPiece) continue;
    if (pc == makePiece(side, kKing)) continue;

    Square effSq = mirror ? mirrorSq(sq) : sq;
    int pi = pieceIndex(pc);
    out.push_back(int(kingSq) * 768 + pi * 64 + int(effSq));
  }
  return out;
}

constexpr int kNnueFeatureCount = 49152;

}  // namespace pocket
