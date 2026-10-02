// Core scalar types for the engine. License-agnostic: this header must not
// import third-party code so the engine core can be relicensed independently.
#pragma once

#include <cstdint>

namespace pocket {

using Bitboard = std::uint64_t;

constexpr int kNumSquares = 64;
constexpr int kNumPieceTypes = 6;
constexpr int kNumColors = 2;
constexpr int kNumPieceTypesNB = 7;  // + NO_PIECE_TYPE sentinel

enum Color : int { kWhite = 0, kBlack = 1, kColorNone = 2 };

enum PieceType : int {
  kNoPieceType = 0,
  kPawn = 1,
  kKnight = 2,
  kBishop = 3,
  kRook = 4,
  kQueen = 5,
  kKing = 6
};

// Piece is encoded as pt + kColorNB*color, leaving index 0 unused so that a
// zeroed piece array reads as "empty".
constexpr int kColorNB = 8;
enum Piece : int {
  kNoPiece = 0,
  kWhitePawn = 1,   kBlackPawn = 9,
  kWhiteKnight = 2, kBlackKnight = 10,
  kWhiteBishop = 3, kBlackBishop = 11,
  kWhiteRook = 4,   kBlackRook = 12,
  kWhiteQueen = 5,  kBlackQueen = 13,
  kWhiteKing = 6,   kBlackKing = 14
};

constexpr Color operator~(Color c) { return Color(c ^ kBlack); }

constexpr Piece makePiece(Color c, PieceType pt) {
  return Piece(int(pt) + kColorNB * int(c));
}

constexpr PieceType typeOf(Piece pc) { return PieceType(int(pc) & 7); }
constexpr Color colorOf(Piece pc) { return Color(int(pc) >> 3); }
// Squares are 0..63 with a1 = 0 and h8 = 63.
enum Square : int {
  kSQ1 = 0,  kSQ2,  kSQ3,  kSQ4,  kSQ5,  kSQ6,  kSQ7,  kSQ8,
  kSQ9,  kSQ10, kSQ11, kSQ12, kSQ13, kSQ14, kSQ15, kSQ16,
  kSQ17, kSQ18, kSQ19, kSQ20, kSQ21, kSQ22, kSQ23, kSQ24,
  kSQ25, kSQ26, kSQ27, kSQ28, kSQ29, kSQ30, kSQ31, kSQ32,
  kSQ33, kSQ34, kSQ35, kSQ36, kSQ37, kSQ38, kSQ39, kSQ40,
  kSQ41, kSQ42, kSQ43, kSQ44, kSQ45, kSQ46, kSQ47, kSQ48,
  kSQ49, kSQ50, kSQ51, kSQ52, kSQ53, kSQ54, kSQ55, kSQ56,
  kSQ57, kSQ58, kSQ59, kSQ60, kSQ61, kSQ62, kSQ63, kSQ64,
  kSQNone = 64
};

enum File : int { kFileA = 0, kFileB, kFileC, kFileD, kFileE, kFileF, kFileG, kFileH, kFileNone };
enum Rank : int { kRank1 = 0, kRank2, kRank3, kRank4, kRank5, kRank6, kRank7, kRank8, kRankNone };

constexpr Square makeSquare(File f, Rank r) { return Square(int(r) * 8 + int(f)); }
constexpr File fileOf(Square s) { return File(int(s) & 7); }
constexpr Rank rankOf(Square s) { return Rank(int(s) >> 3); }

// kSQ1 is a1 and kSQ64 is h8, so the valid range is [kSQ1, kSQNone).
constexpr bool isOk(Square s) { return s >= kSQ1 && s < kSQNone; }

// Mirrors a square vertically (a1 <-> a8). Used for black's perspective.
constexpr Square flipRank(Square s) { return Square(int(s) ^ 56); }
// Mirrors a square horizontally (a1 <-> h1). Used by horizontally mirrored nets.
constexpr Square flipFile(Square s) { return Square(int(s) ^ 7); }
// Rank counted from the given side's point of view: 0 is the side's back rank.
// The flip must stay within 0..7, so it is an XOR with 7, not with 8.
constexpr Rank relativeRank(Color c, Square s) { return Rank(int(rankOf(s)) ^ (7 * int(c))); }
// Relative square from the given side's point of view.
constexpr Square relativeSquare(Color c, Square s) { return Square(int(s) ^ (56 * int(c))); }

constexpr Bitboard squareBB(Square s) { return Bitboard(1) << int(s); }

constexpr bool moreThanOne(Bitboard b) { return (b & (b - 1)) != 0; }

// Castling right bits, indexed by colour.
enum CastlingRight : int {
  kWhiteKingSide = 1,
  kWhiteQueenSide = 2,
  kBlackKingSide = 4,
  kBlackQueenSide = 8
};

// Move: from(6) | to(6) | promotion piece type - kKnight(2) | flags(2)
enum MoveFlag : int { kFlagNormal = 0, kFlagPromotion = 1, kFlagEnPassant = 2, kFlagCastling = 3 };

enum Move : int { kMoveNone = 0 };

constexpr Move makeMove(Square from, Square to) {
  return Move(int(from) | (int(to) << 6));
}

constexpr Move makeMove(Square from, Square to, MoveFlag f) {
  return Move(int(from) | (int(to) << 6) | (int(f) << 14));
}

constexpr Move makePromotion(Square from, Square to, PieceType promo) {
  return Move(int(from) | (int(to) << 6) |
              ((int(promo) - int(kKnight)) << 12) | (int(kFlagPromotion) << 14));
}

constexpr Square fromSq(Move m) { return Square(int(m) & 63); }
constexpr Square toSq(Move m) { return Square((int(m) >> 6) & 63); }
constexpr int promotionTypeField(Move m) { return (int(m) >> 12) & 3; }
constexpr MoveFlag flagOf(Move m) { return MoveFlag((int(m) >> 14) & 3); }

constexpr PieceType promotionType(Move m) { return PieceType(int(kKnight) + promotionTypeField(m)); }
constexpr bool isPromotion(Move m) { return flagOf(m) == kFlagPromotion; }
constexpr bool isEnPassant(Move m) { return flagOf(m) == kFlagEnPassant; }
constexpr bool isCastling(Move m) { return flagOf(m) == kFlagCastling; }

// Value returned by search. Sentinel range keeps mate scores distinct.
constexpr int kValueMate = 32000;
constexpr int kValueMateInMaxPly = kValueMate - 256;
constexpr int kValueInfinity = kValueMate + 1;
constexpr int kValueNone = 32002;

}  // namespace pocket
