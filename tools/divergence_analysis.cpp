// Divergence analysis tool for the ply-98 investigation.
//
// Runs the full engine and the reference search from a single position at
// increasing fixed depths, and also scores every root move individually so
// equal-score tie-breaking can be distinguished from a genuine score split.
//
// Usage:
//   divergence_analysis --fen "FEN" [--maxdepth N] [--movetime N]
#include <cstdio>
#include <cstring>
#include <string>

#include "engine/evaluate.hpp"
#include "engine/movegen.hpp"
#include "engine/position.hpp"
#include "engine/search.hpp"

using namespace pocket;

namespace {

const char* squareName(Square s) {
  static const char* names[64] = {
      "a1", "b1", "c1", "d1", "e1", "f1", "g1", "h1",
      "a2", "b2", "c2", "d2", "e2", "f2", "g2", "h2",
      "a3", "b3", "c3", "d3", "e3", "f3", "g3", "h3",
      "a4", "b4", "c4", "d4", "e4", "f4", "g4", "h4",
      "a5", "b5", "c5", "d5", "e5", "f5", "g5", "h5",
      "a6", "b6", "c6", "d6", "e6", "f6", "g6", "h6",
      "a7", "b7", "c7", "d7", "e7", "f7", "g7", "h7",
      "a8", "b8", "c8", "d8", "e8", "f8", "g8", "h8"};
  return names[int(s)];
}

std::string moveText(Move m) {
  std::string s = std::string(squareName(fromSq(m))) + squareName(toSq(m));
  if (isPromotion(m)) {
    const char p = "  nbrq"[int(promotionType(m)) - int(kKnight)];
    s += p;
  }
  return s;
}

const char* scoreText(int s) {
  static char buf[64];
  if (isMateScore(s)) {
    const int plies = mateDistanceInPlies(s);
    std::snprintf(buf, sizeof(buf), "mate %d (%s)", (plies + 1) / 2,
                  s > 0 ? "win" : "loss");
  } else {
    std::snprintf(buf, sizeof(buf), "cp %d", s);
  }
  return buf;
}

void printPv(const std::vector<Move>& pv) {
  for (size_t i = 0; i < pv.size(); ++i) {
    if (i) std::printf(" ");
    std::printf("%s", moveText(pv[i]).c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string fen;
  int maxDepth = 8;

  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--fen") && i + 1 < argc) {
      fen = argv[++i];
    } else if (!std::strcmp(argv[i], "--maxdepth") && i + 1 < argc) {
      maxDepth = std::atoi(argv[++i]);
    }
  }
  if (fen.empty()) {
    std::printf("usage: divergence_analysis --fen FEN [--maxdepth N]\n");
    return 2;
  }

  Position pos;
  std::string err;
  if (!pos.setFromFen(fen, &err)) {
    std::printf("FEN parse failed: %s\n", err.c_str());
    return 2;
  }

  std::printf("position: %s\n", fen.c_str());
  std::printf("side to move: %s\n", pos.sideToMove() == kWhite ? "White" : "Black");
  std::printf("static eval: %s\n\n", scoreText(evaluate(pos)));

  // --- Full engine, fixed depths -------------------------------------------
  std::printf("=== full engine, fixed depths ===\n");
  for (int d = 1; d <= maxDepth; ++d) {
    Search search(pos);
    const SearchResult r = search.searchDepth(d);
    std::printf("  d%d  score=%-12s best=%-6s nodes=%-9llu qnodes=%-9llu pv=",
                d, scoreText(r.score), moveText(r.best).c_str(),
                (unsigned long long)r.stats.nodes,
                (unsigned long long)r.stats.qnodes);
    printPv(r.pv);
    std::printf("\n");
  }

  // --- Full engine without TT ----------------------------------------------
  std::printf("\n=== full engine, TT disabled ===\n");
  for (int d = 1; d <= maxDepth; ++d) {
    SearchOptions opts;
    opts.useTT = false;
    Search search(pos, opts);
    const SearchResult r = search.searchDepth(d);
    std::printf("  d%d  score=%-12s best=%-6s nodes=%-9llu pv=", d,
                scoreText(r.score), moveText(r.best).c_str(),
                (unsigned long long)r.stats.nodes);
    printPv(r.pv);
    std::printf("\n");
  }

  // --- Full engine without qsearch ------------------------------------------
  std::printf("\n=== full engine, qsearch disabled ===\n");
  for (int d = 1; d <= maxDepth; ++d) {
    SearchOptions opts;
    opts.useQsearch = false;
    Search search(pos, opts);
    const SearchResult r = search.searchDepth(d);
    std::printf("  d%d  score=%-12s best=%-6s nodes=%-9llu pv=", d,
                scoreText(r.score), moveText(r.best).c_str(),
                (unsigned long long)r.stats.nodes);
    printPv(r.pv);
    std::printf("\n");
  }

  // --- Reference search -----------------------------------------------------
  std::printf("\n=== reference search (no TT, no ordering, no qsearch) ===\n");
  for (int d = 1; d <= maxDepth; ++d) {
    Search search(pos);
    const int score = search.searchReference(d);
    std::printf("  d%d  score=%-12s\n", d, scoreText(score));
  }

  // --- Every root move scored individually at a fixed depth -----------------
  // This distinguishes "the two moves have the same score" from "they differ".
  // Each move is searched with a full window so its exact score is known.
  std::printf("\n=== per-move root scores, depth %d (full window) ===\n", maxDepth);
  {
    Search search(pos);
    Move moves[256];
    int count = 0;
    generateLegal(pos, moves, count);
    for (int i = 0; i < count; ++i) {
      pos.doMove(moves[i]);
      const int score = -search.searchReference(maxDepth - 1);
      pos.undoMove(moves[i]);
      std::printf("  %-6s %s\n", moveText(moves[i]).c_str(), scoreText(score));
    }
  }

  return 0;
}
