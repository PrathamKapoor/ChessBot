// Bitboard primitives and PEXT-based magic sliding attack tables.
#pragma once

#include <immintrin.h>

#include <algorithm>
#include <cstdlib>

#include "types.hpp"

namespace pocket {

void initBitboards();

constexpr Bitboard fileBB(int f) {
  return 0x0101010101010101ULL << f;
}
constexpr Bitboard rankBB(int r) { return 0xFFULL << (8 * r); }

extern const Bitboard kFileBB[8];
extern const Bitboard kRankBB[8];

// Bitboard is a plain uint64_t, so the native operators already work. Only the
// Bitboard <-> Square overloads need defining, which is legal because Square is
// an enumeration type.
constexpr Bitboard operator&(Bitboard b, Square s) { return b & squareBB(s); }
constexpr Bitboard operator|(Bitboard b, Square s) { return b | squareBB(s); }
constexpr Bitboard operator^(Bitboard b, Square s) { return b ^ squareBB(s); }
constexpr Bitboard operator|(Square s, Bitboard b) { return b | squareBB(s); }
constexpr Bitboard operator&(Square s, Bitboard b) { return b & squareBB(s); }

inline int popcount(Bitboard b) { return __builtin_popcountll(b); }
inline Square lsb(Bitboard b) { return Square(__builtin_ctzll(b)); }
inline Square msb(Bitboard b) { return Square(63 ^ __builtin_clzll(b)); }
inline Square popLsb(Bitboard& b) {
  const Square s = lsb(b);
  b &= b - 1;
  return s;
}

inline int distance(Square a, Square b) {
  return std::max(std::abs(int(fileOf(a)) - int(fileOf(b))),
                  std::abs(int(rankOf(a)) - int(rankOf(b))));
}

// Pawn attack tables, from each colour's point of view.
extern Bitboard kPawnAttacksBB[kNumColors][kNumSquares];
extern Bitboard kKnightAttacksBB[kNumSquares];
extern Bitboard kKingAttacksBB[kNumSquares];

// --- Magic sliding attacks (PEXT) -------------------------------------------
//
// For each square and direction we keep a `mask` covering the whole ray,
// including the square where the ray leaves the board. A ray is at most 7
// squares long, so every table needs at most 128 entries and PEXT yields a
// directly usable index with no extra shifting.
namespace magic {

constexpr int kNumDirections = 8;

enum Direction : int {
  kNorth = 0, kSouth, kEast, kWest,
  kNorthEast, kNorthWest, kSouthEast, kSouthWest
};

constexpr int kMaxTableSize = 128;

extern Bitboard kMask[kNumSquares][kNumDirections];
extern uint64_t kTable[kNumSquares][kNumDirections][kMaxTableSize];

}  // namespace magic

// PEXT returns a densely packed index in [0, 2^popcount(mask)).
inline Bitboard slidingAttacks(Square s, int dir, Bitboard occ) {
  const Bitboard m = magic::kMask[s][dir];
  return magic::kTable[s][dir][unsigned(_pext_u64(occ, m))];
}

inline Bitboard bishopAttacks(Square s, Bitboard occ) {
  return slidingAttacks(s, magic::kNorthEast, occ) | slidingAttacks(s, magic::kSouthEast, occ) |
         slidingAttacks(s, magic::kNorthWest, occ) | slidingAttacks(s, magic::kSouthWest, occ);
}

inline Bitboard rookAttacks(Square s, Bitboard occ) {
  return slidingAttacks(s, magic::kNorth, occ) | slidingAttacks(s, magic::kSouth, occ) |
         slidingAttacks(s, magic::kEast, occ) | slidingAttacks(s, magic::kWest, occ);
}

inline Bitboard queenAttacks(Square s, Bitboard occ) {
  return bishopAttacks(s, occ) | rookAttacks(s, occ);
}

}  // namespace pocket
