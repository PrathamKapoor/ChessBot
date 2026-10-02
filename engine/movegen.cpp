#include <cstdlib>

#include "movegen.hpp"

namespace pocket {

namespace {

// Offsets expressed in "our perspective": +8 advances toward promotion for the
// side to move regardless of colour.
constexpr int kForward = 8;
constexpr int kForwardLeft = 7;
constexpr int kForwardRight = 9;

inline Rank relativeRank8(Color us) { return us == kWhite ? kRank8 : kRank1; }
// Rank a pawn stands on before it may make a double push: rank 2 for white,
// rank 7 for black.
inline Rank relativeStartRank(Color us) { return us == kWhite ? kRank2 : kRank7; }

// Adds `delta` in our perspective. The 0..63 range check alone is not enough:
// a +7/+9 step from the h-file wraps to the a-file of a rank two further on,
// which is still a legal index. The file check rejects that.
inline bool offsetSquare(Square from, int delta, Color us, Square& out) {
  const int r = int(relativeSquare(us, from)) + delta;
  if (r < 0 || r > 63) return false;
  out = relativeSquare(us, Square(r));
  return std::abs(int(fileOf(out)) - int(fileOf(from))) <= 1;
}

void addPawnMoves(Position& pos, Move* out, int& count, Color us) {
  const Color them = ~us;
  const Bitboard pawns = pos.pieces(us, kPawn);
  const Bitboard enemies = pos.pieces(them);
  const Bitboard empty = ~pos.pieces();
  const Rank promoRank = relativeRank8(us);
  const Rank startRank = relativeStartRank(us);
  const Square ep = pos.epSquare();

  Bitboard b = pawns;
  while (b) {
    const Square from = popLsb(b);

    // Single push, with promotion.
    {
      Square to;
      if (offsetSquare(from, kForward, us, to) && pos.empty(to)) {
        if (rankOf(to) == promoRank) {
          out[count++] = makePromotion(from, to, kQueen);
          out[count++] = makePromotion(from, to, kRook);
          out[count++] = makePromotion(from, to, kBishop);
          out[count++] = makePromotion(from, to, kKnight);
        } else {
          out[count++] = makeMove(from, to);
          // Double push, which may set an en passant square.
          if (rankOf(from) == startRank) {
            Square to2;
            if (offsetSquare(from, 2 * kForward, us, to2) && pos.empty(to2))
              out[count++] = makeMove(from, to2);
          }
        }
      }
    }

    // Diagonal captures, with promotion.
    for (int delta : {kForwardLeft, kForwardRight}) {
      Square to;
      if (!offsetSquare(from, delta, us, to)) continue;
      if (!(enemies & to)) continue;
      if (rankOf(to) == promoRank) {
        out[count++] = makePromotion(from, to, kQueen);
        out[count++] = makePromotion(from, to, kRook);
        out[count++] = makePromotion(from, to, kBishop);
        out[count++] = makePromotion(from, to, kKnight);
      } else {
        out[count++] = makeMove(from, to);
      }
    }

    // En passant capture.
    if (ep != kSQNone) {
      Square to;
      if (offsetSquare(from, kForwardLeft, us, to) && to == ep) {
        out[count++] = makeMove(from, to, kFlagEnPassant);
      } else if (offsetSquare(from, kForwardRight, us, to) && to == ep) {
        out[count++] = makeMove(from, to, kFlagEnPassant);
      }
    }
  }
  (void)empty;
}

// Emits moves for one piece type. King moves are handled separately because
// they need castling too.
void addPieceMoves(Position& pos, Move* out, int& count, Color us, PieceType pt) {
  const Bitboard occ = pos.pieces();
  Bitboard b = pos.pieces(us, pt);

  while (b) {
    const Square from = popLsb(b);
    Bitboard att;
    switch (pt) {
      case kKnight: att = kKnightAttacksBB[from]; break;
      case kBishop: att = bishopAttacks(from, occ); break;
      case kRook: att = rookAttacks(from, occ); break;
      case kQueen: att = bishopAttacks(from, occ) | rookAttacks(from, occ); break;
      default: att = 0; break;
    }
    att &= ~pos.pieces(us);

    while (att) out[count++] = makeMove(from, popLsb(att));
  }
}

void addKingMoves(Position& pos, Move* out, int& count, Color us) {
  const Square from = pos.kingSquare(us);
  Bitboard att = kKingAttacksBB[from] & ~pos.pieces(us);
  while (att) out[count++] = makeMove(from, popLsb(att));

  // Castling is emitted here and validated by Position::legal().
  for (int kingSide = 0; kingSide < 2; ++kingSide) {
    const CastlingRight cr = Position::castlingRight(us, kingSide != 0);
    if (!pos.canCastle(cr)) continue;
    const Square rfrom = Position::castlingRookSquare(us, kingSide != 0);
    if (pos.pieceOn(rfrom) != makePiece(us, kRook)) continue;
    const Square to = Position::castlingKingTo(us, kingSide != 0);
    out[count++] = makeMove(from, to, kFlagCastling);
  }
}

}  // namespace

void generatePseudoLegal(Position& pos, Move* out, int& count) {
  count = 0;
  const Color us = pos.sideToMove();

  addPawnMoves(pos, out, count, us);
  addPieceMoves(pos, out, count, us, kKnight);
  addPieceMoves(pos, out, count, us, kBishop);
  addPieceMoves(pos, out, count, us, kRook);
  addPieceMoves(pos, out, count, us, kQueen);
  addKingMoves(pos, out, count, us);
}

void generateLegal(Position& pos, Move* out, int& count) {
  Move buf[256];
  int n = 0;
  generatePseudoLegal(pos, buf, n);

  count = 0;
  for (int i = 0; i < n; ++i)
    if (pos.legal(buf[i])) out[count++] = buf[i];
}

}  // namespace pocket
