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

// Bitboard is a plain uint64_t. There are deliberately NO operators mixing
// Bitboard with Square: because Square is an enumeration with an underlying
// type of int, an expression like `bb |= sq` silently selects the built-in
// integer `|` and ORs in the square's INDEX as if it were a mask, which is
// wrong and hard to see.
//
// These deleted overloads turn that mistake into a compile error. Without them
// the built-in integer operator is always available and always wins.
Bitboard operator|(Bitboard, Square) = delete;
Bitboard operator&(Bitboard, Square) = delete;
Bitboard operator^(Bitboard, Square) = delete;
Bitboard operator|(Square, Bitboard) = delete;
Bitboard operator&(Square, Bitboard) = delete;
Bitboard& operator|=(Bitboard&, Square) = delete;
Bitboard& operator&=(Bitboard&, Square) = delete;
Bitboard& operator^=(Bitboard&, Square) = delete;

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

// Full rays from each square with nothing on the board. Static masks, so these
// cost a table lookup rather than the eight PEXT probes the magic path needs.
extern Bitboard kDiagonalBB[kNumSquares];
extern Bitboard kOrthogonalBB[kNumSquares];

// Squares strictly between a and b on their common line, or 0 when they share
// no line.
inline Bitboard betweenBB(Square a, Square b) {
  if (a == b) return 0ULL;
  const int ra = int(rankOf(a)), fa = int(fileOf(a));
  const int rb = int(rankOf(b)), fb = int(fileOf(b));

  // The squares strictly between must be restricted to the span of the two
  // squares. Returning the whole rank or file minus the endpoints would include
  // squares outside the pair, which turns a single blocker into two and hides
  // real pins.
  if (ra == rb) {
    Bitboard band = 0;
    for (int f = std::min(fa, fb) + 1; f < std::max(fa, fb); ++f)
      band |= squareBB(makeSquare(File(f), Rank(ra)));
    return band;
  }
  if (fa == fb) {
    Bitboard band = 0;
    for (int r = std::min(ra, rb) + 1; r < std::max(ra, rb); ++r)
      band |= squareBB(makeSquare(File(fa), Rank(r)));
    return band;
  }
  if (std::abs(ra - rb) != std::abs(fa - fb)) return 0ULL;

  Bitboard band = 0;
  for (int r = std::min(ra, rb) + 1; r < std::max(ra, rb); ++r) band |= kRankBB[r];
  return (kDiagonalBB[a] & kDiagonalBB[b]) & band;
}

// True when a, b and c share a rank, file or diagonal.
inline bool aligned(Square a, Square b, Square c) {
  const int ra = int(rankOf(a)), fa = int(fileOf(a));
  const int rb = int(rankOf(b)), fb = int(fileOf(b));
  const int rc = int(rankOf(c)), fc = int(fileOf(c));
  if (ra == rb && ra == rc) return true;
  if (fa == fb && fa == fc) return true;
  if (ra - fa == rb - fb && rb - fb == rc - fc) return true;
  if (ra + fa == rb + fb && rb + fb == rc + fc) return true;
  return false;
}

}  // namespace pocket
