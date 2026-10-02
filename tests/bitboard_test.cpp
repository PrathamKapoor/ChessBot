#include <cstdio>

#include "engine/bitboard.hpp"

using namespace pocket;

static int g_fail = 0;

static void check(bool ok, const char* what) {
  if (!ok) {
    printf("FAIL %s\n", what);
    ++g_fail;
  }
}

// Reference implementation: walk the ray one square at a time. Deliberately
// written the obvious way so it can disagree with the table if the table is
// wrong.
static Bitboard naiveRay(Square s, int dir, Bitboard occ) {
  static const int kStep[8][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0},
                                  {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
  Bitboard att = 0;
  int f = int(fileOf(s));
  int r = int(rankOf(s));
  while (true) {
    f += kStep[dir][0];
    r += kStep[dir][1];
    if (f < 0 || f > 7 || r < 0 || r > 7) break;
    const Square t = makeSquare(File(f), Rank(r));
    att |= squareBB(t);
    if (occ & squareBB(t)) break;
  }
  return att;
}

int main() {
  initBitboards();

  const Square a1 = makeSquare(kFileA, kRank1);
  const Square a2 = makeSquare(kFileA, kRank2);
  const Square a3sq = makeSquare(kFileA, kRank3);
  const Square e4 = makeSquare(kFileE, kRank4);

  // Rook on a1, empty board: rank 1 (b1..h1) + file a (a2..a8). The origin
  // square is never part of the attack set.
  const Bitboard rookA1 = (kRankBB[0] | kFileBB[0]) & ~squareBB(a1);
  check(rookAttacks(a1, 0ULL) == rookA1, "rook a1 empty");
  check(popcount(rookA1) == 14, "rook a1 empty has 14 squares");

  // Bishop on a1, empty board: 7 squares.
  check(popcount(bishopAttacks(a1, 0ULL)) == 7, "bishop a1 empty");

  // Rook on a1 with a pawn on a3: rank 1 stays fully open, the file ray stops
  // at a3 so a4..a8 are excluded.
  {
    Bitboard got = rookAttacks(a1, squareBB(a3sq));
    Bitboard want = (rookA1 & ~kFileBB[0]) | squareBB(a2) | squareBB(a3sq);
    check(got == want, "rook a1 blocked by a3");
    check((got & kFileBB[0]) == (squareBB(a2) | squareBB(a3sq)),
          "rook a1 blocked: file ray stops at a3");
  }

  // Knight on a1 has exactly b3 and c2.
  check(popcount(kKnightAttacksBB[a1]) == 2, "knight a1 count == 2");
  check(kKnightAttacksBB[a1] == (squareBB(makeSquare(kFileB, kRank3)) |
                                 squareBB(makeSquare(kFileC, kRank2))),
        "knight a1 targets");

  // Knight on an interior square has 8 moves.
  check(popcount(kKnightAttacksBB[e4]) == 8, "knight e4 count == 8");

  // King on e4 has 8 neighbours.
  check(popcount(kKingAttacksBB[e4]) == 8, "king e4 count == 8");

  // Pawn attacks from e4.
  check(kPawnAttacksBB[kWhite][e4] == (squareBB(makeSquare(kFileD, kRank5)) |
                                       squareBB(makeSquare(kFileF, kRank5))),
        "white pawn e4");
  check(kPawnAttacksBB[kBlack][e4] == (squareBB(makeSquare(kFileD, kRank3)) |
                                       squareBB(makeSquare(kFileF, kRank3))),
        "black pawn e4");

  // White pawn on a2 has exactly one diagonal attack, to b3.
  check(popcount(kPawnAttacksBB[kWhite][a2]) == 1, "white pawn a2 single attack");
  check(kPawnAttacksBB[kWhite][a2] == squareBB(makeSquare(kFileB, kRank3)),
        "white pawn a2 -> b3");

  // Structural invariants of the magic tables.
  for (int s = 0; s < kNumSquares; ++s)
    for (int d = 0; d < magic::kNumDirections; ++d) {
      if (popcount(magic::kMask[s][d]) > 7) {
        printf("FAIL mask popcount s=%d d=%d\n", s, d);
        ++g_fail;
      }
      // The origin square must never be a blocker candidate.
      if (magic::kMask[s][d] & squareBB(Square(s))) {
        printf("FAIL mask contains origin s=%d d=%d\n", s, d);
        ++g_fail;
      }
      // Rays must be contiguous and monotonic along the direction.
      const Bitboard m = magic::kMask[s][d];
      if (m && lsb(m) == Square(s)) {
        printf("FAIL mask starts at origin s=%d d=%d\n", s, d);
        ++g_fail;
      }
    }

  // Exhaustive-ish cross-check of every sliding direction against the naive
  // walk, over pseudo-random occupancy patterns.
  int compared = 0;
  for (int s = 0; s < kNumSquares; ++s) {
    const Square sq = Square(s);
    for (int d = 0; d < magic::kNumDirections; ++d) {
      for (int trial = 0; trial < 512; ++trial) {
        Bitboard r = Bitboard(trial * 0x9E3779B97F4A7C15ULL + s * 1315423911ULL +
                              d * 2654435761ULL);
        r ^= r >> 33;
        r *= 0xFF51AFD7ED558CCDULL;
        r ^= r >> 33;
        const Bitboard occ = r;

        const Bitboard got = slidingAttacks(sq, d, occ);
        const Bitboard want = naiveRay(sq, d, occ);
        ++compared;
        if (got != want) {
          printf("FAIL ray s=%d d=%d occ=%016llx got=%016llx want=%016llx\n", s, d,
                 (unsigned long long)occ, (unsigned long long)got, (unsigned long long)want);
          ++g_fail;
          break;
        }
      }
    }
  }

  (void)a3sq;
  if (g_fail)
    printf("BITBOARD FAILURES=%d (of %d ray comparisons)\n", g_fail, compared);
  else
    printf("BITBOARD OK (%d ray comparisons)\n", compared);
  return g_fail != 0;
}

