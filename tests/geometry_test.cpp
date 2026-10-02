// Unit tests for the geometry helpers that the legality fast path depends on.
//
// betweenBB() and aligned() were both wrong in ways perft surfaced only as
// small count differences: betweenBB returned whole files for a same-rank pair,
// and a separate version returned the entire rank minus the endpoints. Either
// mistake changes pin detection, so they are tested directly here rather than
// left to be inferred from perft totals.
#include <cstdio>

#include "engine/bitboard.hpp"

using namespace pocket;

namespace {

int g_fail = 0;

void expectEq(const char* what, Bitboard got, Bitboard want) {
  if (got == want) return;
  ++g_fail;
  printf("FAIL %-34s got=%016llx want=%016llx  (got:", what, (unsigned long long)got,
         (unsigned long long)want);
  Bitboard g = got;
  while (g) {
    Square s = popLsb(g);
    printf(" %c%c", 'a' + int(fileOf(s)), '1' + int(rankOf(s)));
  }
  printf(")\n");
}

void expectTrue(const char* what, bool ok) {
  if (ok) return;
  ++g_fail;
  printf("FAIL %-34s expected true\n", what);
}

Square sq(File f, Rank r) { return makeSquare(f, r); }

// Brute-force reference: s lies strictly between a and b when all three share
// one line AND s falls inside the span of the endpoints on that line. Checking
// collinearity alone is not enough, since every square of a rank is collinear.
bool strictlyBetween(Square a, Square b, Square s) {
  const int ra = int(rankOf(a)), fa = int(fileOf(a));
  const int rb = int(rankOf(b)), fb = int(fileOf(b));
  const int rs = int(rankOf(s)), fs = int(fileOf(s));
  const int loR = std::min(ra, rb), hiR = std::max(ra, rb);
  const int loF = std::min(fa, fb), hiF = std::max(fa, fb);
  if (ra == rb && rs == ra) return fs > loF && fs < hiF;                    // rank
  if (fa == fb && fs == fa) return rs > loR && rs < hiR;                    // file
  if (ra - fa == rb - fb && rs - fs == ra - fa) return rs > loR && rs < hiR;  // main diag
  if (ra + fa == rb + fb && rs + fs == ra + fa) return rs > loR && rs < hiR;  // anti diag
  return false;
}

void testBetween() {
  // Explicit cases, chosen because each was a real bug.
  expectEq("between a5 h5", betweenBB(sq(kFileA, kRank5), sq(kFileH, kRank5)),
           squareBB(sq(kFileB, kRank5)) | squareBB(sq(kFileC, kRank5)) |
               squareBB(sq(kFileD, kRank5)) | squareBB(sq(kFileE, kRank5)) |
               squareBB(sq(kFileF, kRank5)) | squareBB(sq(kFileG, kRank5)));

  expectEq("between a5 e5", betweenBB(sq(kFileA, kRank5), sq(kFileE, kRank5)),
           squareBB(sq(kFileB, kRank5)) | squareBB(sq(kFileC, kRank5)) |
               squareBB(sq(kFileD, kRank5)));

  expectEq("between h4 h5 (adjacent)", betweenBB(sq(kFileH, kRank4), sq(kFileH, kRank5)), 0ULL);

  expectEq("between a1 a8", betweenBB(sq(kFileA, kRank1), sq(kFileA, kRank8)),
           squareBB(sq(kFileA, kRank2)) | squareBB(sq(kFileA, kRank3)) |
               squareBB(sq(kFileA, kRank4)) | squareBB(sq(kFileA, kRank5)) |
               squareBB(sq(kFileA, kRank6)) | squareBB(sq(kFileA, kRank7)));

  expectEq("between a1 h8 (diagonal)",
           betweenBB(sq(kFileA, kRank1), sq(kFileH, kRank8)),
           squareBB(sq(kFileB, kRank2)) | squareBB(sq(kFileC, kRank3)) |
               squareBB(sq(kFileD, kRank4)) | squareBB(sq(kFileE, kRank5)) |
               squareBB(sq(kFileF, kRank6)) | squareBB(sq(kFileG, kRank7)));

  expectEq("between a8 h1 (anti-diagonal)",
           betweenBB(sq(kFileA, kRank8), sq(kFileH, kRank1)),
           squareBB(sq(kFileB, kRank7)) | squareBB(sq(kFileC, kRank6)) |
               squareBB(sq(kFileD, kRank5)) | squareBB(sq(kFileE, kRank4)) |
               squareBB(sq(kFileF, kRank3)) | squareBB(sq(kFileG, kRank2)));

  expectEq("between a1 c1 (not adjacent, same rank)",
           betweenBB(sq(kFileA, kRank1), sq(kFileC, kRank1)), squareBB(sq(kFileB, kRank1)));

  expectEq("between a1 b3 (no common line)", betweenBB(sq(kFileA, kRank1), sq(kFileB, kRank3)),
           0ULL);

  // Exhaustive cross-check against the brute-force definition.
  int compared = 0;
  for (int a = 0; a < 64; ++a) {
    for (int b = 0; b < 64; ++b) {
      const Square sa = Square(a), sb = Square(b);
      Bitboard want = 0;
      for (int s = 0; s < 64; ++s) {
        if (s == a || s == b) continue;
        if (strictlyBetween(sa, sb, Square(s))) want |= squareBB(Square(s));
      }
      const Bitboard got = betweenBB(sa, sb);
      ++compared;
      if (got != want) {
        ++g_fail;
        printf("FAIL betweenBB(%c%c,%c%c) got=%016llx want=%016llx\n", 'a' + int(fileOf(sa)),
               '1' + int(rankOf(sa)), 'a' + int(fileOf(sb)), '1' + int(rankOf(sb)),
               (unsigned long long)got, (unsigned long long)want);
        if (g_fail > 5) return;
      }
    }
  }
  printf("  betweenBB: %d pairs cross-checked\n", compared);
}

void testAligned() {
  expectTrue("aligned a5 b5 h5", aligned(sq(kFileB, kRank5), sq(kFileD, kRank5), sq(kFileH, kRank5)));
  expectTrue("aligned e5 e6 e8", aligned(sq(kFileE, kRank5), sq(kFileE, kRank6), sq(kFileE, kRank8)));
  expectTrue("aligned c5 d4 e3", aligned(sq(kFileC, kRank5), sq(kFileD, kRank4), sq(kFileE, kRank3)));
  expectTrue("aligned c5 d6 e7", aligned(sq(kFileC, kRank5), sq(kFileD, kRank6), sq(kFileE, kRank7)));
  expectTrue("aligned a1 a1 a8", aligned(sq(kFileA, kRank1), sq(kFileA, kRank1), sq(kFileA, kRank8)));
  expectTrue("!aligned f4 f3 h4", !aligned(sq(kFileF, kRank4), sq(kFileF, kRank3), sq(kFileH, kRank4)));
  expectTrue("!aligned b5 b6 a5", !aligned(sq(kFileB, kRank5), sq(kFileB, kRank6), sq(kFileA, kRank5)));
  expectTrue("!aligned a1 b2 c4", !aligned(sq(kFileA, kRank1), sq(kFileB, kRank2), sq(kFileC, kRank4)));

  // Every square between two squares must be aligned with them.
  int checked = 0;
  for (int a = 0; a < 64; ++a)
    for (int b = 0; b < 64; ++b) {
      Bitboard mid = betweenBB(Square(a), Square(b));
      while (mid) {
        const Square s = popLsb(mid);
        ++checked;
        if (!aligned(Square(a), s, Square(b))) {
          ++g_fail;
          printf("FAIL aligned(%c%c,%c%c,%c%c) should hold\n", 'a' + int(fileOf(Square(a))),
                 '1' + int(rankOf(Square(a))), 'a' + int(fileOf(s)), '1' + int(rankOf(s)),
                 'a' + int(fileOf(Square(b))), '1' + int(rankOf(Square(b))));
          if (g_fail > 5) return;
        }
      }
    }
  printf("  aligned: %d between-square relations checked\n", checked);
}

// The static ray masks must partition each square into orthogonal and diagonal
// rays, and must never include the origin.
void testRayTables() {
  for (int s = 0; s < 64; ++s) {
    const Square sqr = Square(s);
    const Bitboard o = kOrthogonalBB[sqr];
    const Bitboard d = kDiagonalBB[sqr];
    if (o & d) {
      ++g_fail;
      printf("FAIL rays overlap at %c%c\n", 'a' + int(fileOf(sqr)), '1' + int(rankOf(sqr)));
    }
    if ((o | d) & squareBB(sqr)) {
      ++g_fail;
      printf("FAIL rays include origin at %c%c\n", 'a' + int(fileOf(sqr)), '1' + int(rankOf(sqr)));
    }
    if (popcount(o) + popcount(d) != popcount(queenAttacks(sqr, 0ULL))) {
      ++g_fail;
      printf("FAIL ray coverage at %c%c\n", 'a' + int(fileOf(sqr)), '1' + int(rankOf(sqr)));
    }
  }
}

}  // namespace

int main() {
  setvbuf(stdout, NULL, _IONBF, 0);
  initBitboards();

  testBetween();
  testAligned();
  testRayTables();

  if (g_fail) {
    printf("GEOMETRY FAILURES=%d\n", g_fail);
    return 1;
  }
  printf("GEOMETRY OK\n");
  return 0;
}
