// perft: the correctness gate for move generation.
//
// Counts leaf nodes at a fixed depth and compares against the reference values
// published on the Chess Programming Wiki. A single wrong move anywhere shows
// up here as a mismatched count.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/movegen.hpp"
#include "engine/position.hpp"

using namespace pocket;

namespace {

struct PerftCase {
  const char* name;
  const char* fen;
  // Perft counts for depths 1..6; 0 means "no published value, skip".
  unsigned long long expected[7];
};

const PerftCase kCases[] = {
    {"startpos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
     {1ULL, 20ULL, 400ULL, 8902ULL, 197281ULL, 4865609ULL, 119060324ULL}},
    {"kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
     {1ULL, 48ULL, 2039ULL, 97862ULL, 4085603ULL, 193690690ULL, 0ULL}},
    {"endgame-rook", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
     {1ULL, 14ULL, 191ULL, 2812ULL, 43238ULL, 674624ULL, 11030083ULL}},
    {"promo-castle", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
     {1ULL, 6ULL, 264ULL, 9467ULL, 422333ULL, 15833292ULL, 0ULL}},
    {"promo-heavy", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
     {1ULL, 44ULL, 1486ULL, 62379ULL, 2103487ULL, 89941194ULL, 0ULL}},
    {"positional", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
     {1ULL, 46ULL, 2079ULL, 89890ULL, 3894594ULL, 164075551ULL, 0ULL}},
};

unsigned long long perft(Position& pos, int depth) {
  if (depth <= 0) return 1ULL;
  Move buf[256];
  int n = 0;
  generateLegal(pos, buf, n);
  if (depth == 1) return (unsigned long long)n;

  unsigned long long total = 0;
  for (int i = 0; i < n; ++i) {
    pos.doMove(buf[i]);
    total += perft(pos, depth - 1);
    pos.undoMove(buf[i]);
  }
  return total;
}

int g_fail = 0;

void moveToUci(Move m, char* out) {
  out[0] = char('a' + int(fileOf(fromSq(m))));
  out[1] = char('1' + int(rankOf(fromSq(m))));
  out[2] = char('a' + int(fileOf(toSq(m))));
  out[3] = char('1' + int(rankOf(toSq(m))));
  if (!isPromotion(m)) {
    out[4] = '\0';
    return;
  }
  switch (promotionType(m)) {
    case kQueen: out[4] = 'q'; break;
    case kRook: out[4] = 'r'; break;
    case kBishop: out[4] = 'b'; break;
    default: out[4] = 'n'; break;
  }
  out[5] = '\0';
}

// Emits one CSV line per node at exactly `depth` plies: the move sequence and
// that node's legal move count. Diffing against the same enumeration from
// python-chess pinpoints the first diverging move sequence exactly.
void emitTree(Position& pos, int depth, char* path, int len) {
  Move buf[256];
  int n = 0;
  generateLegal(pos, buf, n);

  if (depth == 0) {
    path[len] = '\0';
    printf("%s,%d\n", path, n);
    return;
  }

  char mv[8];
  for (int i = 0; i < n; ++i) {
    moveToUci(buf[i], mv);
    const int seg = int(std::strlen(mv));
    memcpy(path + len, mv, size_t(seg));
    pos.doMove(buf[i]);
    emitTree(pos, depth - 1, path, len + seg);
    pos.undoMove(buf[i]);
  }
}

}  // namespace

int main(int argc, char** argv) {
  int maxDepth = 5;
  bool listDivides = false;
  bool emitTreeMode = false;
  const char* onlyFen = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--depth") && i + 1 < argc) maxDepth = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--divides")) listDivides = true;
    else if (!std::strcmp(argv[i], "--tree")) emitTreeMode = true;
    else if (!std::strcmp(argv[i], "--fen") && i + 1 < argc) onlyFen = argv[++i];
  }

  unsigned long long grandTotal = 0;
  const auto t0 = std::chrono::steady_clock::now();

  // --fen runs one arbitrary position instead of the built-in suite.
  std::vector<PerftCase> cases;
  if (onlyFen) {
    cases.push_back({"custom", onlyFen, {0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL}});
  } else {
    cases.assign(std::begin(kCases), std::end(kCases));
  }

  for (const PerftCase& c : cases) {
    // The divides/tree modes emit machine-readable lines, so the human-readable
    // case banner must be suppressed or it prefixes the first line.
    if (!listDivides && !emitTreeMode) printf("%-14s ", c.name);
    for (int d = 1; d <= maxDepth; ++d) {
      Position pos;
      std::string err;
      if (!pos.setFromFen(c.fen, &err)) {
        printf("\n  FEN PARSE FAILED: %s\n", err.c_str());
        ++g_fail;
        break;
      }

      if (emitTreeMode) {
        // Only the requested depth, so the output lines up with
        // tools/reference_tree.py which takes a single depth.
        if (d != maxDepth) continue;
        char path[512];
        emitTree(pos, d, path, 0);
        continue;
      }

      if (listDivides) {
        Move buf[256];
        int n = 0;
        generateLegal(pos, buf, n);
        // CSV: case,depth,move,count -- consumed by tools/perft_diff.ps1.
        for (int i = 0; i < n; ++i) {
          pos.doMove(buf[i]);
          const unsigned long long sub = perft(pos, d - 1);
          pos.undoMove(buf[i]);
          // UCI-style promotion suffix (q/r/b/n) so promotions stay distinct.
          char suffix = ' ';
          if (isPromotion(buf[i])) {
            switch (promotionType(buf[i])) {
              case kQueen: suffix = 'q'; break;
              case kRook: suffix = 'r'; break;
              case kBishop: suffix = 'b'; break;
              default: suffix = 'n'; break;
            }
          }
          printf("%s,%d,%c%c%c%c%c,%llu\n", onlyFen ? "custom" : c.name, d,
                 'a' + int(fileOf(fromSq(buf[i]))), '1' + int(rankOf(fromSq(buf[i]))),
                 'a' + int(fileOf(toSq(buf[i]))), '1' + int(rankOf(toSq(buf[i]))), suffix,
                 sub);
        }
        continue;
      }

      const unsigned long long got = perft(pos, d);
      grandTotal += got;

      if (c.expected[d] != 0) {
        if (got != c.expected[d]) {
          printf("\n  depth %d: FAIL got %llu expected %llu\n", d, got, c.expected[d]);
          ++g_fail;
          break;
        }
      } else {
        printf("[%d]=%llu ", d, got);
        continue;
      }
      printf("[%d]=%llu ok ", d, got);
    }
    if (!listDivides) printf("\n");
  }

  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  printf("\nnodes=%llu  time=%.3fs  nps=%.0f\n", grandTotal, secs,
         secs > 0 ? grandTotal / secs : 0.0);
  printf(g_fail ? "PERFT FAILURES=%d\n" : "PERFT OK\n", g_fail);
  return g_fail != 0;
}
