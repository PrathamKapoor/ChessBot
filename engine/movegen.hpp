// Move generation.
//
// This first version generates pseudo-legal moves and filters them through
// Position::legal(). That is deliberately the slow, obviously-correct path: the
// perft suite is the gate, and a faster legality filter (pins, check evasions,
// discovered checkers) is only worth adding once perft is green.
#pragma once

#include "position.hpp"
#include "types.hpp"

namespace pocket {

// Appends every pseudo-legal move for the side to move to `out`, in no
// particular order. The caller is responsible for legality filtering.
void generatePseudoLegal(Position& pos, Move* out, int& count);

// Appends every legal move. This is what search and perft use.
void generateLegal(Position& pos, Move* out, int& count);

inline int legalMoveCount(Position& pos) {
  Move buf[256];
  int n = 0;
  generateLegal(pos, buf, n);
  return n;
}

}  // namespace pocket
