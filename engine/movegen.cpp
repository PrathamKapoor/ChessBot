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
      if (!(enemies & squareBB(to))) continue;
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

void generateLegalReference(Position& pos, Move* out, int& count) {
  Move buf[256];
  int n = 0;
  generatePseudoLegal(pos, buf, n);

  count = 0;
  for (int i = 0; i < n; ++i)
    if (pos.legal(buf[i])) out[count++] = buf[i];
}

namespace {

// The king may not move onto a square the opponent attacks. Only the king's
// own origin and the captured square change the occupancy, and a piece standing
// on the destination does not block a ray aimed at that destination, so the
// occupancy used here is pieces() ^ from.
inline bool kingStepIsSafe(const Position& pos, Square from, Square to) {
  return (pos.attackersTo(to, pos.pieces() ^ squareBB(from)) & pos.pieces(~pos.sideToMove())) == 0;
}

// Castling keeps its own requirements: right held, rook present, path empty,
// king not currently in check, and no square the king crosses attacked. This
// runs at most twice per position so the straightforward form is fine.
bool castlingIsLegal(Position& pos, Color us, Square from, Square to) {
  const Color them = ~us;
  const bool kingSide = to > from;
  if (!pos.canCastle(Position::castlingRight(us, kingSide))) return false;

  const Square rfrom = Position::castlingRookSquare(us, kingSide);
  if (pos.pieceOn(rfrom) != makePiece(us, kRook)) return false;
  if (pos.inCheck()) return false;

  const int step = kingSide ? 1 : -1;

  Bitboard needEmpty = 0;
  for (Square s = Square(int(from) + step); s != rfrom; s = Square(int(s) + step))
    needEmpty |= squareBB(s);
  if (pos.pieces() & needEmpty) return false;

  // The king only walks from its origin to the destination: f1 and g1 on the
  // king side, d1 and c1 on the queen side.
  const Square kto = Position::castlingKingTo(us, kingSide);
  Bitboard kingPath = 0;
  for (Square s = kto; s != from; s = Square(int(s) - step)) kingPath |= squareBB(s);

  // The rook is dropped from the occupancy: it is our own piece and cannot
  // attack us, but it does block sliders along the rank.
  const Bitboard occ = pos.pieces() ^ squareBB(rfrom);
  while (kingPath) {
    const Square s = popLsb(kingPath);
    if (pos.attackersTo(s, occ) & pos.pieces(them)) return false;
  }
  return true;
}

}  // namespace

void generateLegal(Position& pos, Move* out, int& count) {
  const Color us = pos.sideToMove();
  const Square ksq = pos.kingSquare(us);
  const Bitboard checkers = pos.checkers();

  // Computed once per position. Only our own pieces matter for the pin test,
  // and the mask also contains the pinning sliders, which is harmless because
  // moves are only generated from squares holding our pieces.
  const Bitboard pinned = pos.blockersForKing(us);

  // When in check, a non-king move must answer the check: capture the checker,
  // or block the ray if the checker is a slider.
  Bitboard checkTargets = 0;
  if (checkers) {
    checkTargets = checkers;
    if (moreThanOne(checkers)) {
      // Double check: nothing but the king can answer it, and only by moving to
      // a square the opponent does not attack.
      count = 0;
      Bitboard att = kKingAttacksBB[ksq] & ~pos.pieces(us);
      while (att) {
        const Square to = popLsb(att);
        if (kingStepIsSafe(pos, ksq, to)) out[count++] = makeMove(ksq, to);
      }
      return;
    }
    const Piece checker = pos.pieceOn(lsb(checkers));
    if (typeOf(checker) == kBishop || typeOf(checker) == kRook || typeOf(checker) == kQueen)
      checkTargets |= betweenBB(ksq, lsb(checkers));
  }

  Move pseudo[256];
  int nPseudo = 0;
  generatePseudoLegal(pos, pseudo, nPseudo);

  count = 0;
  for (int i = 0; i < nPseudo; ++i) {
    const Move m = pseudo[i];
    const Square from = fromSq(m);
    const Square to = toSq(m);
    const PieceType pt = typeOf(pos.pieceOn(from));

    // Castling must be recognised before the generic king case below, because
    // its origin square holds the king too. It needs the path-empty and
    // not-castling-through-check rules that a plain king step does not have.
    if (isCastling(m)) {
      if (castlingIsLegal(pos, us, from, to)) out[count++] = m;
      continue;
    }

    if (pt == kKing) {
      if (kingStepIsSafe(pos, from, to)) out[count++] = m;
      continue;
    }

    if (isEnPassant(m)) {
      // Two pawns leave the board on the same rank, which can uncover a slider
      // in ways the pin mask cannot express (the captured pawn may itself be
      // the blocker). En passant is rare enough to keep the exact test.
      if (pos.legal(m)) out[count++] = m;
      continue;
    }

    // The pin test: a piece that is not pinned cannot expose the king, and a
    // pinned piece is safe as long as it stays on the pinning line. Move
    // generation already guarantees the destination is empty or enemy-occupied,
    // and physically guarantees a slider cannot pass through the king or the
    // pinning slider, so staying on the line is sufficient.
    if ((pinned & squareBB(from)) && !aligned(from, to, ksq)) continue;

    // When in check, the move must also answer the check.
    if (checkers && !(checkTargets & squareBB(to))) continue;

    out[count++] = m;
  }
}

bool checkLegalAgrees(Position& pos) {
  Move fast[256], ref[256];
  int nf = 0, nr = 0;
  generateLegal(pos, fast, nf);
  generateLegalReference(pos, ref, nr);
  if (nf != nr) return false;
  for (int i = 0; i < nf; ++i)
    if (fast[i] != ref[i]) return false;
  return true;
}

}  // namespace pocket
