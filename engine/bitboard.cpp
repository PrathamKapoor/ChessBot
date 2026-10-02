#include "bitboard.hpp"

#include <cstdlib>

namespace pocket {

const Bitboard kFileBB[8] = {
    0x0101010101010101ULL, 0x0202020202020202ULL, 0x0404040404040404ULL, 0x0808080808080808ULL,
    0x1010101010101010ULL, 0x2020202020202020ULL, 0x4040404040404040ULL, 0x8080808080808080ULL};
const Bitboard kRankBB[8] = {
    0xFFULL, 0xFF00ULL, 0xFF0000ULL, 0xFF000000ULL,
    0xFF00000000ULL, 0xFF0000000000ULL, 0xFF000000000000ULL, 0xFF00000000000000ULL};

Bitboard kPawnAttacksBB[kNumColors][kNumSquares];
Bitboard kKnightAttacksBB[kNumSquares];
Bitboard kKingAttacksBB[kNumSquares];

namespace magic {
Bitboard kMask[kNumSquares][magic::kNumDirections];
uint64_t kTable[kNumSquares][magic::kNumDirections][magic::kMaxTableSize];
}  // namespace magic

namespace {

struct Step {
  int df, dr;
};

// One step per direction, in (file, rank) deltas.
constexpr Step kRayStep[magic::kNumDirections] = {
    {0, 1},   {0, -1},  {1, 0},   {-1, 0},   // N, S, E, W
    {1, 1},   {-1, -1}, {1, -1},  {-1, 1}};  // NE, NW, SE, SW

constexpr Step kKnightStep[8] = {{1, 2},  {2, 1},   {2, -1},  {1, -2},
                                 {-1, -2}, {-2, -1}, {-2, 1},  {-1, 2}};
constexpr Step kKingStep[8] = {{0, 1},  {1, 1},   {1, 0},   {1, -1},
                               {0, -1}, {-1, -1}, {-1, 0},  {-1, 1}};

// Attacks along one ray from `s`, including the first blocker square.
Bitboard rayAttacks(Square s, int dir, Bitboard occ) {
  Bitboard att = 0;
  int f = int(fileOf(s));
  int r = int(rankOf(s));
  while (true) {
    f += kRayStep[dir].df;
    r += kRayStep[dir].dr;
    if (f < 0 || f > 7 || r < 0 || r > 7) break;
    const Square t = makeSquare(File(f), Rank(r));
    att |= squareBB(t);
    if (occ & t) break;
  }
  return att;
}

void initStepTable(Bitboard table[kNumSquares], const Step step[8]) {
  for (int i = 0; i < kNumSquares; ++i) {
    Bitboard att = 0;
    for (int j = 0; j < 8; ++j) {
      const int f = (i & 7) + step[j].df;
      const int r = (i >> 3) + step[j].dr;
      if (f < 0 || f > 7 || r < 0 || r > 7) continue;
      att |= squareBB(makeSquare(File(f), Rank(r)));
    }
    table[i] = att;
  }
}

void initMagic(Square s, int dir) {
  const int df = kRayStep[dir].df;
  const int dr = kRayStep[dir].dr;
  int f = int(fileOf(s));
  int r = int(rankOf(s));

  // The mask is the whole ray, including the square where the ray leaves the
  // board, so that a blocker anywhere on the ray is represented.
  Bitboard mask = 0;
  while (true) {
    f += df;
    r += dr;
    if (f < 0 || f > 7 || r < 0 || r > 7) break;
    mask |= squareBB(makeSquare(File(f), Rank(r)));
  }

  magic::kMask[s][dir] = mask;

  const int bits = popcount(mask);
  if (bits > 7) std::abort();  // kMaxTableSize assumption violated

  // For each occupancy subset of `mask`, record the attacks it produces.
  const int subsets = 1 << bits;
  for (int sub = 0; sub < subsets; ++sub) {
    Bitboard occ = 0;
    Bitboard remaining = mask;
    int b = 0;
    while (remaining) {
      const Bitboard lsbBit = remaining & -remaining;
      if (sub & (1 << b)) occ |= lsbBit;
      remaining ^= lsbBit;
      ++b;
    }
    magic::kTable[s][dir][unsigned(_pext_u64(occ, mask))] = rayAttacks(s, dir, occ);
  }
}

}  // namespace

void initBitboards() {
  static bool done = false;
  if (done) return;
  done = true;

  initStepTable(kKnightAttacksBB, kKnightStep);
  initStepTable(kKingAttacksBB, kKingStep);

  for (int i = 0; i < kNumSquares; ++i) {
    for (Color c : {kWhite, kBlack}) {
      Bitboard res = 0;
      const int f = i & 7;
      const int r = (i >> 3) + (c == kWhite ? 1 : -1);
      if (r >= 0 && r <= 7) {
        if (f > 0) res |= squareBB(makeSquare(File(f - 1), Rank(r)));
        if (f < 7) res |= squareBB(makeSquare(File(f + 1), Rank(r)));
      }
      kPawnAttacksBB[c][i] = res;
    }
  }

  for (int s = 0; s < kNumSquares; ++s)
    for (int d = 0; d < magic::kNumDirections; ++d) initMagic(Square(s), d);
}

}  // namespace pocket
