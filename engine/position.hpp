// Board state: piece placement, castling, en passant, clocks and Zobrist keys.
#pragma once

#include <cassert>
#include <cstdint>
#include <string>

#include "bitboard.hpp"
#include "types.hpp"

namespace pocket {

// Everything needed to restore a position when a move is unmade.
struct StateInfo {
  Piece capturedPiece = kNoPiece;
  int castlingRights = 0;
  Square epSquare = kSQNone;
  int rule50 = 0;
  int pliesFromNull = 0;
  // Total non-pawn, non-king material for both sides. Null-move pruning is
  // unsafe in zugzwang, which is approximated as having no such material.
  int nonPawnMaterial = 0;
  uint64_t key = 0;
  uint64_t pawnKey = 0;
  Move move = kMoveNone;
};

void initZobrist();

// Squares a pawn of colour `c` standing on `s` would attack.
inline Bitboard pawnAttacksFrom(Square s, Color c) {
  const Bitboard sq = squareBB(s);
  if (c == kWhite) {
    Bitboard a = 0;
    if (fileOf(s) > kFileA) a |= sq << 7;
    if (fileOf(s) < kFileH) a |= sq << 9;
    return a;
  }
  Bitboard a = 0;
  if (fileOf(s) > kFileA) a |= sq >> 9;
  if (fileOf(s) < kFileH) a |= sq >> 7;
  return a;
}

// True if a piece of type `pt` on `from` attacks `to` under occupancy `occ`.
inline bool attacksSquare(PieceType pt, Square from, Square to, Bitboard occ) {
  switch (pt) {
    case kPawn: return (pawnAttacksFrom(from, kWhite) & to) != 0;
    case kKnight: return (kKnightAttacksBB[from] & to) != 0;
    case kBishop: return (bishopAttacks(from, occ) & to) != 0;
    case kRook: return (rookAttacks(from, occ) & to) != 0;
    case kQueen: return (queenAttacks(from, occ) & to) != 0;
    default: return (kKingAttacksBB[from] & to) != 0;
  }
}

class Position {
 public:
  Position() { clear(); }

  void clear();

  // Returns false and fills `err` when the FEN is malformed.
  bool setFromFen(const std::string& fen, std::string* err = nullptr);
  std::string fen() const;

  void doMove(Move m);
  void undoMove(Move m);
  void doNullMove();
  void undoNullMove();

  Color sideToMove() const { return sideToMove_; }
  int gamePly() const { return gamePly_; }
  int rule50Count() const { return state_->rule50; }
  uint64_t key() const { return state_->key; }
  uint64_t pawnKey() const { return state_->pawnKey; }
  int nonPawnMaterial() const { return state_->nonPawnMaterial; }

  Square epSquare() const { return state_->epSquare; }

  Piece pieceOn(Square s) const { return board_[int(s)]; }
  bool empty(Square s) const { return board_[int(s)] == kNoPiece; }

  Bitboard pieces() const { return byColorBB_[0] | byColorBB_[1]; }
  Bitboard pieces(Color c) const { return byColorBB_[int(c)]; }
  Bitboard pieces(PieceType pt) const { return byTypeBB_[int(pt)]; }
  Bitboard pieces(Color c, PieceType pt) const {
    return byColorBB_[int(c)] & byTypeBB_[int(pt)];
  }
  Bitboard pieces(PieceType a, PieceType b) const {
    return byTypeBB_[int(a)] | byTypeBB_[int(b)];
  }
  Bitboard pieces(PieceType a, PieceType b, PieceType c) const {
    return byTypeBB_[int(a)] | byTypeBB_[int(b)] | byTypeBB_[int(c)];
  }
  Bitboard pieces(Color c, PieceType a, PieceType b) const {
    return byColorBB_[int(c)] & (byTypeBB_[int(a)] | byTypeBB_[int(b)]);
  }

  Square kingSquare(Color c) const {
    const Bitboard b = byTypeBB_[int(kKing)] & byColorBB_[int(c)];
    assert(b != 0);
    return lsb(b);
  }

  Bitboard checkers() const { return checkers_; }
  bool inCheck() const { return checkers_ != 0; }
  bool inCheck(Color c) const { return (checkers_ & byColorBB_[int(c)]) != 0; }

  // Pieces of any colour attacking `s` under occupancy `occ`.
  Bitboard attackersTo(Square s, Bitboard occ) const;

  bool canCastle(CastlingRight cr) const { return (state_->castlingRights & int(cr)) != 0; }
  // Bit layout is 1 = white king, 2 = white queen, 4 = black king, 8 = black
  // queen, matching the kCastlingRight* constants.
  static CastlingRight castlingRight(Color c, bool kingSide) {
    return CastlingRight(kingSide ? (c == kWhite ? 1 : 4) : (c == kWhite ? 2 : 8));
  }
  static Square castlingRookSquare(Color c, bool kingSide) {
    return kingSide ? makeSquare(kFileH, Rank(int(c) * 7))
                    : makeSquare(kFileA, Rank(int(c) * 7));
  }
  static Square castlingKingTo(Color c, bool kingSide) {
    return kingSide ? makeSquare(kFileG, Rank(int(c) * 7))
                    : makeSquare(kFileC, Rank(int(c) * 7));
  }
  // The rook lands on the square the king passes over: f1 on the king side,
  // d1 on the queen side.
  static Square castlingRookTo(Color c, bool kingSide) {
    return kingSide ? makeSquare(kFileF, Rank(int(c) * 7))
                    : makeSquare(kFileD, Rank(int(c) * 7));
  }

  // legal() and givesCheck() play and unplay the move, so they are not const.
  bool legal(Move m);
  bool givesCheck(Move m);
  bool captureOrPromotion(Move m) const;
  Piece movedPiece(Move m) const { return board_[int(fromSq(m))]; }
  Piece capturedPiece() const { return state_->capturedPiece; }

  int repetitionCount() const;
  bool hasNonPawnMaterial(Color c) const;

 private:
  void putPiece(Piece pc, Square s);
  void removePiece(Square s);
  void movePiece(Square from, Square to);
  void disallowCastling(Square s);
  void setCheckers();
  void doCastling(Color us, Square kfrom, Square kto, Square rfrom, Square rto);

  Piece board_[kNumSquares];
  Bitboard byTypeBB_[kNumPieceTypesNB];
  Bitboard byColorBB_[kNumColors];
  int castlingRightsMask_[kNumSquares];
  Color sideToMove_;
  int gamePly_;
  Bitboard checkers_;
  StateInfo stateStack_[1024];
  StateInfo* state_;

  std::string toFenString() const;
};

// Material value of a piece type, ignoring square.
inline int pieceValue(PieceType pt) {
  switch (pt) {
    case kPawn: return 100;
    case kKnight: return 320;
    case kBishop: return 330;
    case kRook: return 500;
    case kQueen: return 900;
    default: return 0;
  }
}

}  // namespace pocket
