// Classical tapered evaluation.
//
// Score convention: the returned value is from the SIDE TO MOVE's perspective,
// in centipawns, and is symmetric under negation. Search uses it directly in a
// negamax frame, so a positive score always favours the side to move.
//
// The evaluation is deliberately built from small, separately testable terms
// rather than one opaque expression. Each term returns a (middlegame, endgame)
// pair and the two are interpolated by the game phase, which is what lets the
// same tables describe "the knight belongs on c3 in the middlegame" and "the
// knight belongs on d5 in the endgame".
//
// Nothing here is random, incremental or lazy: the same position always scores
// the same, which is what makes search regression tests meaningful.
#pragma once

#include "position.hpp"
#include "types.hpp"

namespace pocket {

// Score from the side to move's perspective, in centipawns.
int evaluate(const Position& pos);

// Material values used by the evaluator. Piece-square tables are stored
// relative to these so the two never drift apart. Indexed by PieceType, so the
// arrays are 7 entries wide (kNoPieceType .. kKing).
constexpr int kMgValue[kNumPieceTypesNB] = {0, 82, 337, 365, 477, 1025, 0};
constexpr int kEgValue[kNumPieceTypesNB] = {0, 94, 281, 297, 512, 936, 0};

// Phase weights and the phase value of a full board.
constexpr int kPhaseWeight[kNumPieceTypesNB] = {0, 0, 1, 1, 2, 4, 0};
constexpr int kMaxPhase = 24;

// Small bonus for having the move. It is applied after the perspective flip, so
// it is the one part of the score that is deliberately not colour-symmetric.
constexpr int kTempoMg = 12;

// Game phase in [0, kMaxPhase]. 24 means every piece is on the board.
int gamePhase(const Position& pos);

// Exposed for tests and for the ablation mode: the individual terms, all from
// White's perspective and returned as (middlegame, endgame) pairs.
struct TaperedScore {
  int mg = 0;
  int eg = 0;
  TaperedScore& operator+=(const TaperedScore& o) { mg += o.mg; eg += o.eg; return *this; }
};

TaperedScore evalMaterial(const Position& pos);
TaperedScore evalPieceSquares(const Position& pos);
TaperedScore evalPawnStructure(const Position& pos);
TaperedScore evalPieces(const Position& pos);
TaperedScore evalKingSafety(const Position& pos);
TaperedScore evalThreats(const Position& pos);
TaperedScore evalSpace(const Position& pos);
TaperedScore evalTempo(const Position& pos);

// Static exchange evaluation, in centipawns from the side to move's point of
// view. Positive means the capture sequence wins material for the mover.
//
// Full implementation of the recursive swap algorithm. Exposed because move
// ordering needs it and because it is worth testing on its own.
int seeCapture(const Position& pos, Move m);

}  // namespace pocket
