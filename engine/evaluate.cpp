#include "evaluate.hpp"

#include <algorithm>
#include <cstdlib>

namespace pocket {

namespace {

using T = TaperedScore;

// --- Piece-square tables ----------------------------------------------------
//
// Written in the usual visual order, rank 8 first, and flipped at load time so
// that index 0 is a1. Values are the PeSTO set (Friederich), scaled so that a
// pawn is worth about 100 and the material constants in evaluate.hpp match.
// They are a starting point, not a tuned result: tuning is a later phase and
// must be measured with matches, not assumed.

constexpr int kPawnMg[64] = {
    0,   0,   0,   0,   0,   0,    0,    0,   //
    98, 134, 61,  95,  68, 126,  34,  -11,  //
    -6,  7,   26,  31,  65, 56,   25,  -20,  //
    -14, 13,  6,   21,  23, 12,   17,  -23,  //
    -27, -2,  -5,  12,  17, 6,    10,  -25,  //
    -26, -4,  -4,  -10, 3,  3,    33,  -12,  //
    -35, -1,  -20, -23, -15, 24,  38,  -22,  //
    0,   0,   0,   0,   0,   0,    0,    0};
constexpr int kPawnEg[64] = {
    0,   0,   0,   0,   0,   0,   0,   0,     //
    178, 173, 158, 134, 147, 132, 165, 187,   //
    -4,  5,   10,  13,  8,   9,   8,   -4,    //
    -5,  4,   7,   6,   9,   1,   3,   -7,    //
    -4,  0,   -5,  -1,  -11, -2,  -8,  -3,    //
    2,   -5,  -3,  -5,  -9,  -12, -8,  -16,   //
    -3,  1,   -10, -6,  1,   -9,  -9,  -17,   //
    0,   0,   0,   0,   0,   0,   0,   0};
constexpr int kKnightMg[64] = {
    -167, -89, -34, -49, 61,  -97, -15, -107,  //
    -73,  -41, 72,  36,  23,  62,  7,   -17,   //
    -47,  60,  37,  65,  84,  129, 73,  44,    //
    -9,   17,  19,  53,  37,  69,  18,  22,    //
    -13,  4,   16,  13,  28,  19,  21,  -8,    //
    -23,  -9,  12,  10,  19,  17,  25,  -16,   //
    -29,  -53, -12, -3,  -1,  18,  -14, -19,   //
    -105, -21, -58, -33, -17, -28, -19, -23};
constexpr int kKnightEg[64] = {
    -58, -38, -13, -28, -31, -27, -63, -99,  //
    -25, -8,  -25, -2,  -9,  -25, -24, -52,  //
    -24, -20, 10,  9,   -1,  -9,  -19, -41,  //
    -17, 3,   22,  22,  22,  11,  8,   -18,  //
    -18, -6,  16,  25,  16,  17,  4,   -18,  //
    -23, -3,  -1,  15,  10,  -3,  -20, -22,  //
    -42, -20, -10, -5,  -2,  -20, -23, -44,  //
    -29, -51, -23, -15, -22, -18, -50, -64};
constexpr int kBishopMg[64] = {
    -29, 4,   -82, -37, -25, -42, 7,   -8,   //
    -26, 16,  -18, -13, 30,  59,  18,  -47,  //
    -16, 37,  43,  40,  35,  50,  37,  -2,   //
    -4,  5,   19,  50,  37,  37,  7,   -2,   //
    -6,  13,  13,  26,  34,  12,  10,  4,    //
    0,   15,  15,  15,  14,  27,  18,  10,   //
    4,   15,  16,  0,   7,   21,  33,  1,    //
    -33, -3,  -14, -21, -13, -12, -39, -21};
constexpr int kBishopEg[64] = {
    -14, -21, -11, -8,  -7,  -9,  -17, -24,  //
    -8,  -4,  7,   -12, -3,  -13, -4,  -14,  //
    2,   -8,  0,   -1,  -2,  6,   0,   4,    //
    -3,  9,   12,  9,   14,  10,  3,   2,    //
    -6,  3,   13,  19,  7,   10,  -3,  -9,   //
    -12, -3,  8,   10,  13,  3,   -7,  -15,  //
    -14, -18, -7,  -1,  4,   -9,  -15, -27,  //
    -23, -9,  -23, -5,  -9,  -16, -5,  -17};
constexpr int kRookMg[64] = {
    32,  42,  32,  51,  63,  9,  31,  43,  //
    27,  32,  58,  62,  80,  67,  26,  44,  //
    -5,  19,  26,  36,  17,  45,  61,  16,  //
    -24, -11, 7,   26,  24,  35,  -8,  -20, //
    -36, -26, -12, -1,  9,   -7,  6,   -23, //
    -45, -25, -16, -17, 3,   0,   -5,  -33, //
    -44, -16, -20, -9,  -1,  11,  -6,  -71, //
    -19, -13, 1,   17,  16,  7,   -37, -26};
constexpr int kRookEg[64] = {
    13,  10,  18,  15,  12,  12,  8,   5,   //
    11,  13,  13,  11,  -3,  3,   8,   3,   //
    7,   7,   7,   5,   4,   -3,  -5,  -3,  //
    4,   3,   13,  1,   2,   1,   -1,  2,   //
    3,   5,   8,   4,   -5,  -6,  -8,  -11, //
    -4,  0,   -5,  -1,  -7,  -12, -8,  -16, //
    -6,  -6,  0,   2,   -9,  -9,  -11, -3,  //
    -9,  2,   3,   -1,  -5,  -13, 4,   -20};
constexpr int kQueenMg[64] = {
    -28, 0,   29,  12, 59,  44,  43,  45,  //
    -24, -39, -5,  1,  -16, 57, 28,  54,  //
    -13, -17, 7,   8,  29,  56, 47,  57,  //
    -27, -27, -16, -16, -1,  17, -2,  1,   //
    -9,  -26, -9,  -10, -2, -4,  3,   -3,  //
    -14, 2,   -11, -2,  -5,  2,  14,  5,   //
    -35, -8,  11,  2,   8,   15, -3,  1,   //
    -1,  -18, -9,  10, -15, -25, -31, -50};
constexpr int kQueenEg[64] = {
    -9,  22,  22,  27,  27,  19,  10, 20,   //
    -17, 20,  32,  41,  58,  25,  30,  0,   //
    -20, 6,   9,   49,  47,  35,  19,  9,   //
    3,   22,  24,  45,  57,  40,  57, 36,   //
    -18, 28,  19,  47,  31,  34,  39, 23,   //
    -16, -27, 15,  6,   9,   17,  10,  5,   //
    -22, -23, -30, -16, -16, -23, -36, -32, //
    -33, -28, -22, -27, -1,  -27, -23, -42};
constexpr int kKingMg[64] = {
    -65, 23,  16,  -15, -56, -34, 21,  23,  //
    29,  -1,  -20,  -7,  -8,  -4, -38, -29,  //
    -9,  24,  2,   -16, -20, 6,   22, -22,  //
    -17, -20, -12, -27, -30, -25, -14, -36,  //
    -49, -1,  -27, -39, -46, -44, -33, -51,  //
    -14, -14, -22, -46, -44, -30, -15, -27,  //
    1,   -7,  -8,  -29, -56, -12, -6,  10,   //
    -67, -21, 11,  -6,  -2,  1,   12,  -53};
constexpr int kKingEg[64] = {
    -74, -35, -18, -18, -11, 15,   4,   -17,  //
    -12, 17,  14,  17,  17,  38,  23,   11,   //
    10,  17,  23,  15,  20,  45,  44,  13,   //
    -8,  22,  24,  27,  26,  33,  26,   3,    //
    -18, -4,  21,  24,  27,  23,   9,   -11,  //
    -19,  -3,  11,  21,  23,  16,   7,   -9,   //
    -27, -11,   4,  13,  14,   4,   -5,  -17,  //
    -53, -34, -21, -11, -28, -14, -24, -43};

// Loaded tables, indexed a1 = 0. Flipping happens once at startup.
struct PstTables {
  // mg[pt][s] is the value for a White piece on square s. Black reads the same
  // array through flipRank(), because a Black piece on square s is worth to
  // Black exactly what a White piece on flipRank(s) is worth to White.
  int mg[kNumPieceTypesNB][kNumSquares];
  int eg[kNumPieceTypesNB][kNumSquares];
  bool ready = false;
};
PstTables g_pst;

// Mirror a square across the board. a1 is index 0, so this is 63 - s.
constexpr Square flipSquare(Square s) { return Square(int(kSQ64) - int(s)); }

// Note: the tables above are written rank 8 first, so a value written for ranks
// 8..1 has to be re-indexed by rank alone before it can be read at a1 = 0.
// That is flipRank(), which types.hpp already provides. Mirroring the file as
// well would be a 180-degree rotation, which is not a chess symmetry, and it
// silently breaks colour-symmetry of the whole evaluation.

void loadPst() {
  if (g_pst.ready) return;
  const int* src[kNumPieceTypesNB] = {nullptr, kPawnMg, kKnightMg, kBishopMg,
                                       kRookMg, kQueenMg, kKingMg};
  const int* srcEg[kNumPieceTypesNB] = {nullptr, kPawnEg, kKnightEg, kBishopEg,
                                         kRookEg, kQueenEg, kKingEg};
  for (int pt = kPawn; pt <= kKing; ++pt) {
    for (int s = 0; s < kNumSquares; ++s) {
      // src is written rank 8 first, so re-index by rank alone.
      g_pst.mg[pt][s] = src[pt][flipRank(Square(s))];
      g_pst.eg[pt][s] = srcEg[pt][flipRank(Square(s))];
    }
  }
  g_pst.ready = true;
}

// --- Weights ----------------------------------------------------------------
//
// Every non-table term in one place, so tuning later is a single edit and the
// current values can be read in one screen. Units are centipawns.

constexpr int kBishopPairMg = 30;
constexpr int kIsolatedPawnMg = 12, kIsolatedPawnEg = 18;
constexpr int kDoubledPawnMg = 10, kDoubledPawnEg = 22;
constexpr int kBackwardPawnMg = 8, kBackwardPawnEg = 10;
// Passed-pawn reward, indexed by the pawn's rank RELATIVE TO ITS OWN SIDE
// (row 0 is a pawn still on its home rank) rather than by absolute rank. That
// ordering is what makes the term colour-symmetric: a White pawn on a2 and a
// Black pawn on a7 are the same relative rank and must read the same row, and
// reading an absolute-rank table would give them different bonuses.
constexpr int kPassedPawnMg[kNumSquares] = {
    0,   0,   0,   0,   0,   0,   0,   0,     //
    5,   10,  15,  20,  25,  30,  35,  40,    //
    15,  25,  35,  45,  55,  60,  65,  70,    //
    30,  45,  60,  75,  90,  95,  100, 105,   //
    55,  75,  95,  115, 135, 140, 145, 150,   //
    90,  120, 145, 170, 195, 200, 205, 210,   //
    140, 175, 205, 235, 260, 265, 270, 275,   //
    200, 240, 275, 310, 340, 345, 350, 355};
constexpr int kPassedPawnEg[kNumSquares] = {
    0,   0,   0,   0,   0,   0,   0,   0,    //
    60,  65,  70,  75,  80,  85,  90,  95,   //
    110, 120, 130, 140, 150, 155, 160, 165,  //
    170, 190, 210, 230, 250, 255, 260, 265,  //
    240, 270, 300, 330, 360, 365, 370, 375,  //
    330, 375, 420, 465, 510, 515, 520, 525,  //
    470, 530, 590, 650, 710, 715, 720, 725,  //
    650, 730, 810, 890, 970, 975, 980, 985};

// Index into the tables above for a pawn of colour c standing on s.
constexpr int passedIndex(Color c, Square s) {
  const int relRank = c == kWhite ? int(rankOf(s)) : 7 - int(rankOf(s));
  return relRank * 8 + int(fileOf(s));
}

// Mobility: score per square a piece can reach, counted after excluding our own
// pieces, the to-square and pawns from the king zone.
constexpr int kMobilityMg[kNumPieceTypesNB] = {0, 0, 4, 4, 3, 2, 0};
constexpr int kMobilityEg[kNumPieceTypesNB] = {0, 0, 4, 5, 4, 4, 0};

constexpr int kKingAttackWeight[kNumPieceTypesNB] = {0, 0, 20, 20, 40, 80, 0};
constexpr int kKingShieldMg = 12;   // per own pawn in front of a castled king
constexpr int kKingOpenFileMg = 28; // per open file next to the king
constexpr int kKingSafetyScale = 2; // global damping, keeps the term small

constexpr int kHangingMg = 12, kHangingEg = 16;   // undefended enemy piece we attack
constexpr int kMinorAttackedMg = 18, kMinorAttackedEg = 26;
constexpr int kRookAttackedMg = 32, kRookAttackedEg = 40;
constexpr int kQueenAttackedMg = 40, kQueenAttackedEg = 50;

constexpr int kSpaceMg = 2;  // per own pawn/square in the enemy half

// Bitboards of the half of the board each side is trying to invade, for the
// space term. White advances towards rank 8, so its enemy half is ranks 6 and 7
// (bits 40..55); Black's is ranks 2 and 3 (bits 8..23).
constexpr Bitboard kEnemyHalfW = 0x0000FF0000000000ULL;
constexpr Bitboard kEnemyHalfB = 0x0000000000FF0000ULL;

// Forward pawn span per colour, used to find passed and backward pawns.
Bitboard forwardSpan(Color c, Square s) {
  const Bitboard sq = squareBB(s);
  return c == kWhite ? (sq << 8) | (sq << 16) | (sq << 24) : (sq >> 8) | (sq >> 16) | (sq >> 24);
}
Bitboard adjacentFilesBB(File f) {
  Bitboard b = 0;
  if (f > kFileA) b |= fileBB(int(f) - 1);
  if (f < kFileH) b |= fileBB(int(f) + 1);
  return b;
}
Bitboard pawnAttacksBBAll(Color c) {
  Bitboard b = 0;
  for (int s = 0; s < kNumSquares; ++s) b |= pawnAttacksFrom(Square(s), c);
  return b;
}

}  // namespace

// --- Public terms -----------------------------------------------------------

int gamePhase(const Position& pos) {
  int phase = 0;
  for (int pt = kPawn; pt <= kQueen; ++pt) phase += popcount(pos.pieces(PieceType(pt))) * kPhaseWeight[pt];
  return std::max(0, std::min(kMaxPhase, phase));
}

TaperedScore evalMaterial(const Position& pos) {
  TaperedScore s;
  for (int pt = kPawn; pt <= kKing; ++pt) {
    // Signed per colour: pieces(PieceType) spans both colours, so counting it
    // directly would add every piece on the board to White's tally.
    const int n = popcount(pos.pieces(kWhite, PieceType(pt))) -
                  popcount(pos.pieces(kBlack, PieceType(pt)));
    s.mg += n * kMgValue[pt];
    s.eg += n * kEgValue[pt];
  }
  return s;
}

TaperedScore evalPieceSquares(const Position& pos) {
  loadPst();
  TaperedScore s;
  for (int pt = kPawn; pt <= kKing; ++pt) {
    const Bitboard w = pos.pieces(kWhite, PieceType(pt));
    const Bitboard b = pos.pieces(kBlack, PieceType(pt));
    Bitboard x = w;
    while (x) {
      const Square sq = popLsb(x);
      s.mg += g_pst.mg[pt][int(sq)];
      s.eg += g_pst.eg[pt][int(sq)];
    }
    x = b;
    while (x) {
      const Square sq = popLsb(x);
      s.mg -= g_pst.mg[pt][flipRank(sq)];
      s.eg -= g_pst.eg[pt][flipRank(sq)];
    }
  }
  return s;
}

TaperedScore evalPawnStructure(const Position& pos) {
  TaperedScore s;

  // Bishop pair.
  if (popcount(pos.pieces(kWhite, kBishop)) >= 2) s.mg += kBishopPairMg;
  if (popcount(pos.pieces(kBlack, kBishop)) >= 2) s.mg -= kBishopPairMg;

  for (Color c : {kWhite, kBlack}) {
    const Bitboard ours = pos.pieces(c, kPawn);
    const Bitboard theirs = pos.pieces(~c, kPawn);
    const int sign = c == kWhite ? 1 : -1;
    Bitboard b = ours;
    while (b) {
const Square sq = popLsb(b);
      const File f = fileOf(sq);
      const Bitboard adjFiles = adjacentFilesBB(f);

      // Squares ahead of the pawn on its own file, and the same file plus its
      // neighbours.
      const Bitboard front = forwardSpan(c, sq);
      const Bitboard files = fileBB(int(f)) | adjFiles;

      // Isolated: no friendly pawn anywhere on an adjacent file. Checking only
      // the starting rank would call a2 isolated while b1's neighbour is a pawn
      // on b4, and it would also break colour-symmetry, since the starting rank
      // is a White-and-Black-specific square.
      const bool isolated = (ours & adjFiles) == 0;
      // Doubled: another friendly pawn shares this file.
      const bool doubled = (ours & fileBB(int(f))) != squareBB(sq);

      // Passed: no enemy pawn ahead on this file or an adjacent one.
      const bool passed = (theirs & files & front) == 0;

      // Backward: supported from no side, while an enemy pawn bears down the
      // file. Already-forward pawns count as supported for this purpose.
      const Bitboard behindOrSame = c == kWhite ? (squareBB(sq) | (squareBB(sq) >> 8))
                                                : (squareBB(sq) | (squareBB(sq) << 8));
      const bool backward = !isolated && (ours & adjFiles & behindOrSame) == 0 &&
                            (pawnAttacksBBAll(c) & theirs & front) != 0;

      if (isolated) {
        s.mg += sign * kIsolatedPawnMg;
        s.eg += sign * kIsolatedPawnEg;
      }
      if (doubled) {
        s.mg += sign * kDoubledPawnMg;
        s.eg += sign * kDoubledPawnEg;
      }
      if (backward && !passed) {
        s.mg += sign * kBackwardPawnMg;
        s.eg += sign * kBackwardPawnEg;
      }
      if (passed) {
        const int idx = passedIndex(c, sq);
        s.mg += sign * kPassedPawnMg[idx];
        s.eg += sign * kPassedPawnEg[idx];
      }
    }
  }
  return s;
}

TaperedScore evalPieces(const Position& pos) {
  TaperedScore s;
  const Bitboard occ = pos.pieces();

  for (Color c : {kWhite, kBlack}) {
    const Bitboard ours = pos.pieces(c);
    const int sign = c == kWhite ? 1 : -1;

    for (int pt = kKnight; pt <= kQueen; ++pt) {
      Bitboard b = pos.pieces(c, PieceType(pt));
      while (b) {
        const Square sq = popLsb(b);
        Bitboard att;
        switch (pt) {
          case kKnight: att = kKnightAttacksBB[sq]; break;
          case kBishop: att = bishopAttacks(sq, occ); break;
          case kRook: att = rookAttacks(sq, occ); break;
          default: att = queenAttacks(sq, occ); break;
        }
        // A piece may move to any square it attacks that is not occupied by one
        // of its own pieces. Enemy-occupied squares count, empty squares count,
        // own-occupied squares do not.
        att &= ~ours;
        s.mg += sign * popcount(att) * kMobilityMg[pt];
        s.eg += sign * popcount(att) * kMobilityEg[pt];
      }
    }
  }
  return s;
}

TaperedScore evalKingSafety(const Position& pos) {
  TaperedScore s;
  const Bitboard occ = pos.pieces();

  for (Color c : {kWhite, kBlack}) {
    const Color them = ~c;
    const int sign = c == kWhite ? 1 : -1;
    const Square ksq = pos.kingSquare(c);
    const Bitboard zone = kKingAttacksBB[ksq] | squareBB(ksq);

    int attackUnits = 0;
    Bitboard b = pos.pieces(them);
    while (b) {
      const Square sq = popLsb(b);
      if (!(zone & squareBB(sq))) continue;
      const PieceType pt = typeOf(pos.pieceOn(sq));
      const int w = kKingAttackWeight[pt];
      if (w == 0) continue;
      // Only attackers that actually bear on the zone contribute.
      Bitboard att;
      switch (pt) {
        case kPawn: att = pawnAttacksFrom(sq, them); break;
        case kKnight: att = kKnightAttacksBB[sq]; break;
        case kBishop: att = bishopAttacks(sq, occ); break;
        case kRook: att = rookAttacks(sq, occ); break;
        default: att = queenAttacks(sq, occ); break;
      }
      if (att & zone) attackUnits += w;
    }

    int mg = -attackUnits * kKingSafetyScale;

    // Pawn shield: for a king still on its back rank, reward own pawns directly
    // in front and penalise a completely open neighbouring file.
    if (rankOf(ksq) == (c == kWhite ? kRank1 : kRank8)) {
      const File kf = fileOf(ksq);
      const Rank ahead = c == kWhite ? kRank2 : kRank7;
      for (int df = -1; df <= 1; ++df) {
        const File f = File(int(kf) + df);
        if (f < kFileA || f > kFileH) continue;
        if (pos.pieceOn(makeSquare(f, ahead)) == makePiece(c, kPawn))
          mg += kKingShieldMg;
        else if (!(occ & fileBB(int(f))))
          mg -= kKingOpenFileMg;
      }
    }

    // King safety is a middlegame concern only; the endgame weight stays zero so
    // the term cannot distort endgames.
    s.mg += sign * mg;
  }
  return s;
}

TaperedScore evalThreats(const Position& pos) {
  TaperedScore s;
  const Bitboard occ = pos.pieces();

  for (Color c : {kWhite, kBlack}) {
    const Color them = ~c;
    const int sign = c == kWhite ? 1 : -1;

    // Enemy pieces we attack that are not defended by another enemy piece.
    Bitboard b = pos.pieces(them);
    while (b) {
      const Square sq = popLsb(b);
      const PieceType pt = typeOf(pos.pieceOn(sq));
      if (pt == kPawn || pt == kKing) continue;
      const Bitboard defenders = pos.attackersTo(sq, occ ^ squareBB(sq)) & pos.pieces(them);
      const bool attackedByUs = (pos.attackersTo(sq, occ ^ squareBB(sq)) & pos.pieces(c)) != 0;
      if (attackedByUs && defenders == 0) {
        s.mg += sign * kHangingMg;
        s.eg += sign * kHangingEg;
      }
    }

    // Our pieces they attack, weighted by value.
    b = pos.pieces(c);
    while (b) {
      const Square sq = popLsb(b);
      const PieceType pt = typeOf(pos.pieceOn(sq));
      if (pt == kPawn || pt == kKing) continue;
      const Bitboard attackers = pos.attackersTo(sq, occ ^ squareBB(sq)) & pos.pieces(them);
      if (attackers == 0) continue;
      const bool defended = (pos.attackersTo(sq, occ ^ squareBB(sq)) & pos.pieces(c)) != 0;
      if (defended) continue;
      if (pt == kQueen) {
        s.mg += sign * kQueenAttackedMg;
        s.eg += sign * kQueenAttackedEg;
      } else if (pt == kRook) {
        s.mg += sign * kRookAttackedMg;
        s.eg += sign * kRookAttackedEg;
      } else {
        s.mg += sign * kMinorAttackedMg;
        s.eg += sign * kMinorAttackedEg;
      }
    }
  }
  return s;
}

TaperedScore evalSpace(const Position& pos) {
  TaperedScore s;
  // Own pawns and pieces in the enemy half count as space.
  const Bitboard wSpace = pos.pieces(kWhite) & kEnemyHalfW;
  const Bitboard bSpace = pos.pieces(kBlack) & kEnemyHalfB;
  s.mg = popcount(wSpace) * kSpaceMg - popcount(bSpace) * kSpaceMg;
  return s;
}

TaperedScore evalTempo(const Position& pos) {
  // Tempo belongs to the side to move, not to White, so it cannot live in the
  // White-perspective sum. It returns zero here and evaluate() adds it after the
  // perspective flip; the function exists so the weight is still documented and
  // testable in one place.
  (void)pos;
  return TaperedScore{};
}

// --- Static exchange evaluation ---------------------------------------------

namespace {

// Value of whatever stands on `to`, plus any promotion gain for the mover.
int captureValue(const Position& pos, Move m) {
  const Square from = fromSq(m);
  const Square to = toSq(m);
  if (isEnPassant(m)) return pieceValue(kPawn);

  int gain = 0;
  const Piece victim = pos.pieceOn(to);
  if (victim != kNoPiece) gain = pieceValue(typeOf(victim));
  if (isPromotion(m)) gain += pieceValue(promotionType(m)) - pieceValue(kPawn);
  (void)from;
  return gain;
}

// Least valuable piece of colour `c` that attacks `to` under `occ`.
//
// The intersection with `occ` matters: attackersTo() derives pawn, knight and
// king attackers from the stored board rather than from the occupancy passed
// in, so without it a piece already consumed by the swap sequence would be
// handed out again and the loop would never terminate.
//
// Kings are excluded because a king cannot be captured; letting one into the
// swap sequence would contribute a zero-value capture and corrupt the sum.
Square leastValuableAttacker(const Position& pos, Square to, Bitboard occ, Color c) {
  const Bitboard attackers = pos.attackersTo(to, occ) & pos.pieces(c) & occ &
                             ~pos.pieces(kKing);
  static const PieceType kOrder[5] = {kPawn, kKnight, kBishop, kRook, kQueen};
  for (PieceType pt : kOrder) {
    const Bitboard bb = attackers & pos.pieces(pt);
    if (bb) return lsb(bb);
  }
  return kSQNone;
}

}  // namespace

int seeCapture(const Position& pos, Move m) {
  if (!pos.captureOrPromotion(m)) return 0;
  if (isCastling(m)) return 0;  // castling captures nothing

  const Square from = fromSq(m);
  const Square to = toSq(m);

  Bitboard occ = pos.pieces() ^ squareBB(from) ^ squareBB(to);
  if (isEnPassant(m)) occ ^= squareBB(makeSquare(fileOf(to), rankOf(from)));

  // Walk the swap sequence, cheapest recapture first.
  //
  // The mover wins the victim but exposes their own piece, so the occupant of
  // the square only becomes a loss once a recapture actually happens. Each time
  // the square changes hands, the piece that was sitting there dies and the new
  // occupant is put at risk, which is what `pending` tracks.
  constexpr int kMaxSwaps = 32;
  int vals[kMaxSwaps + 1];
  int n = 0;
  vals[n++] = captureValue(pos, m);

  int pending = isPromotion(m) ? pieceValue(promotionType(m))
                               : pieceValue(typeOf(pos.pieceOn(from)));

  Color c = ~pos.sideToMove();
  while (n < kMaxSwaps + 1) {
    const Square a = leastValuableAttacker(pos, to, occ, c);
    if (a == kSQNone) break;
    occ ^= squareBB(a);
    vals[n++] = pending;
    pending = pieceValue(typeOf(pos.pieceOn(a)));
    c = ~c;
  }

  // The mover wins vals[0], loses vals[1], wins vals[2], and so on, so the answer
  // is the alternating sum. Folding it in from the back is the compact way to
  // write that recurrence.
  int score = 0;
  for (int i = n - 1; i >= 0; --i) score = vals[i] - score;
  return score;
}

// --- Entry point ------------------------------------------------------------

int evaluate(const Position& pos) {
  TaperedScore total;  // every term below is from White's perspective
  total += evalMaterial(pos);
  total += evalPieceSquares(pos);
  total += evalPawnStructure(pos);
  total += evalPieces(pos);
  total += evalKingSafety(pos);
  total += evalThreats(pos);
  total += evalSpace(pos);

  const int phase = gamePhase(pos);
  const int white = (total.mg * phase + total.eg * (kMaxPhase - phase)) / kMaxPhase;

  // Flip to the side to move, then hand out the tempo bonus. The tempo has to be
  // applied after the flip: folding it into the White-perspective sum would make
  // evaluate(b) differ from -evaluate(w) on an identical board.
  return pos.sideToMove() == kWhite ? white + kTempoMg : -white + kTempoMg;
}

}  // namespace pocket
