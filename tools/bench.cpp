// bench: performance measurement and component profiling for the chess core.
//
// Two distinct jobs:
//
//   --bench    Headline traversal speed. This is the number that matters, and
//              it is measured under exactly the same conditions before and
//              after each optimization so the comparison is fair.
//
//   --profile  Attribute cycles to individual components. Each node's own cost
//              is measured as a window that excludes its children, so the
//              reported shares sum to 100%. Cycles come from rdtsc; the ~25
//              cycle overhead per measurement is small next to the several
//              thousand cycles a position costs.
//
// The profile also reports the move mix and the special-position mix, because
// the cost distribution depends entirely on which move kinds occur.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <x86intrin.h>

#include "engine/movegen.hpp"
#include "engine/position.hpp"

using namespace pocket;

namespace {

struct Case {
  const char* name;
  const char* fen;
  int depth;
};

const Case kCases[] = {
    {"startpos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5},
    {"kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4},
    {"endgame-rook", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 6},
    {"promo-castle", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 5},
    {"promo-heavy", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 5},
    {"positional", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 5},
};

// Positions that exercise the rare move kinds, profiled separately so their
// per-move cost is visible instead of being averaged into thousands of
// ordinary positions.
const Case kSpecialCases[] = {
    {"in-check-single", "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5Q2/PPPP1PPP/RNB1K1NR w KQkq - 4 4", 4},
    {"in-check-double", "4k3/8/8/8/8/2n5/8/R3K2R b KQ - 0 1", 4},
    {"ep-available", "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3", 4},
    {"ep-pin-risk", "8/8/8/K2pP2r/8/8/8/7k w - d6 0 1", 6},
    {"promotions", "r3k2r/1P4P1/8/8/8/8/1p4p1/R3K2R w KQkq - 0 1", 5},
    {"castling-both", "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", 5},
    {"middlegame-open", "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQK2R w KQkq - 4 7", 4},
    {"pinned-heavy", "rnbqkb1r/ppp2ppp/5n2/3pp3/3P4/2N1PN2/PPP2PPP/R1BQKB1R w KQkq - 0 1", 4},
};

// Volatile sink so the optimizer cannot delete measured work.
volatile std::uint64_t g_sink = 0;

unsigned long long countNodes(Position& pos, int depth) {
  if (depth <= 0) return 1ULL;
  Move buf[256];
  int n = 0;
  generateLegal(pos, buf, n);
  if (depth == 1) return (unsigned long long)n;
  unsigned long long total = 0;
  for (int i = 0; i < n; ++i) {
    pos.doMove(buf[i]);
    total += countNodes(pos, depth - 1);
    pos.undoMove(buf[i]);
  }
  return total;
}

double runBench(Position& pos, int depth) {
  const auto t0 = std::chrono::steady_clock::now();
  const unsigned long long nodes = countNodes(pos, depth);
  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  g_sink += nodes;
  printf("  nodes=%llu  time=%.4fs  nps=%.0f\n", nodes, secs, secs > 0 ? nodes / secs : 0.0);
  return secs > 0 ? nodes / secs : 0.0;
}

struct Stats {
  unsigned long long positions = 0;
  unsigned long long pseudoMoves = 0;
  unsigned long long legalMoves = 0;
  unsigned long long cyclesChecked = 0;  // nodeOwn, excludes children

  unsigned long long cPseudoGen = 0;
  unsigned long long cLegalOther = 0;
  unsigned long long cLegalKing = 0;
  unsigned long long cLegalEp = 0;
  unsigned long long cLegalCastle = 0;
  unsigned long long cDoUndo = 0;

  unsigned long long posInCheck = 0, posWithEp = 0, posWithCastling = 0, posDoubleCheck = 0;
  unsigned long long mvKing = 0, mvPawn = 0, mvPromotion = 0, mvEp = 0, mvCastle = 0;
};

void addStats(Stats& a, const Stats& b) {
  auto add = [](unsigned long long& x, unsigned long long y) { x += y; };
  add(a.positions, b.positions);
  add(a.pseudoMoves, b.pseudoMoves);
  add(a.legalMoves, b.legalMoves);
  add(a.cyclesChecked, b.cyclesChecked);
  add(a.cPseudoGen, b.cPseudoGen);
  add(a.cLegalOther, b.cLegalOther);
  add(a.cLegalKing, b.cLegalKing);
  add(a.cLegalEp, b.cLegalEp);
  add(a.cLegalCastle, b.cLegalCastle);
  add(a.cDoUndo, b.cDoUndo);
  add(a.posInCheck, b.posInCheck);
  add(a.posDoubleCheck, b.posDoubleCheck);
  add(a.posWithEp, b.posWithEp);
  add(a.posWithCastling, b.posWithCastling);
  add(a.mvKing, b.mvKing);
  add(a.mvPawn, b.mvPawn);
  add(a.mvPromotion, b.mvPromotion);
  add(a.mvEp, b.mvEp);
  add(a.mvCastle, b.mvCastle);
}

// Measures one node's own work, then recurses. The node window deliberately
// closes before descending so that children never inflate the parent's share.
//
// `useReference` selects Position::legal() per pseudo-legal move (the Phase 1
// path) instead of the current generateLegal(). Profiling the wrong one of these
// reports a bottleneck that no longer exists, so both are selectable.
void profileNode(Position& pos, int depth, Stats& st, bool useReference = false) {
  const unsigned long long nodeStart = __rdtsc();

  Move legalBuf[256];
  int nLegal = 0;

  if (useReference) {
    // Split the old path so it is still comparable: pseudo-legal generation,
    // then a legal() call per move.
    Move pseudoBuf[256];
    int nPseudo = 0;
    const unsigned long long c0 = __rdtsc();
    generatePseudoLegal(pos, pseudoBuf, nPseudo);
    const unsigned long long c1 = __rdtsc();
    st.cPseudoGen += c1 - c0;
    st.pseudoMoves += (unsigned long long)nPseudo;
    st.positions++;

    const Bitboard checkers = pos.checkers();
    if (checkers) st.posInCheck++;
    if (moreThanOne(checkers)) st.posDoubleCheck++;
    if (pos.epSquare() != kSQNone) st.posWithEp++;
    if (pos.canCastle(kWhiteKingSide) || pos.canCastle(kWhiteQueenSide) ||
        pos.canCastle(kBlackKingSide) || pos.canCastle(kBlackQueenSide))
      st.posWithCastling++;

    for (int i = 0; i < nPseudo; ++i) {
      const Move m = pseudoBuf[i];
      const unsigned long long t0 = __rdtsc();
      const bool ok = pos.legal(m);
      const unsigned long long t1 = __rdtsc();
      if (isCastling(m)) {
        st.cLegalCastle += t1 - t0;
        ++st.mvCastle;
      } else if (isEnPassant(m)) {
        st.cLegalEp += t1 - t0;
        ++st.mvEp;
      } else if (typeOf(pos.pieceOn(fromSq(m))) == kKing) {
        st.cLegalKing += t1 - t0;
        ++st.mvKing;
      } else {
        st.cLegalOther += t1 - t0;
        if (isPromotion(m)) ++st.mvPromotion;
        else if (typeOf(pos.pieceOn(fromSq(m))) == kPawn) ++st.mvPawn;
      }
      if (ok) legalBuf[nLegal++] = m;
    }
    st.legalMoves += (unsigned long long)nLegal;
  } else {
    // The production path: one call that does generation and legality.
    const unsigned long long c0 = __rdtsc();
    generateLegal(pos, legalBuf, nLegal);
    const unsigned long long c1 = __rdtsc();
    st.cPseudoGen += c1 - c0;  // "movegen" bucket: generation plus legality
    st.pseudoMoves += (unsigned long long)nLegal;
    st.positions++;
    st.legalMoves += (unsigned long long)nLegal;

    const Bitboard checkers = pos.checkers();
    if (checkers) st.posInCheck++;
    if (moreThanOne(checkers)) st.posDoubleCheck++;
    if (pos.epSquare() != kSQNone) st.posWithEp++;
    if (pos.canCastle(kWhiteKingSide) || pos.canCastle(kWhiteQueenSide) ||
        pos.canCastle(kBlackKingSide) || pos.canCastle(kBlackQueenSide))
      st.posWithCastling++;
  }

  // do/undo over the legal moves. This includes check detection, which is
  // recomputed inside doMove.
  const unsigned long long d0 = __rdtsc();
  for (int i = 0; i < nLegal; ++i) {
    pos.doMove(legalBuf[i]);
    pos.undoMove(legalBuf[i]);
  }
  const unsigned long long d1 = __rdtsc();
  st.cDoUndo += d1 - d0;

  st.cyclesChecked += __rdtsc() - nodeStart;

  if (depth <= 0) return;

  for (int i = 0; i < nLegal; ++i) {
    pos.doMove(legalBuf[i]);
    profileNode(pos, depth - 1, st, useReference);
    pos.undoMove(legalBuf[i]);
  }
}

double share(unsigned long long part, unsigned long long whole) {
  return whole ? 100.0 * double(part) / double(whole) : 0.0;
}

void printStats(const Stats& st, const char* title) {
  const double inv = double(st.positions) ? 1.0 / double(st.positions) : 0.0;
  const unsigned long long T = st.cyclesChecked;
  const unsigned long long legalSum =
      st.cLegalOther + st.cLegalKing + st.cLegalEp + st.cLegalCastle;

  printf("\n%s\n", title);
  printf("  positions %llu   pseudo/pos %.2f   legal/pos %.2f   cycles/pos %.1f\n",
         st.positions, double(st.pseudoMoves) * inv, double(st.legalMoves) * inv,
         double(T) * inv);

  printf("  %-32s %11s %8s\n", "component", "cyc/pos", "share");
  printf("  %-32s %11.1f %7.1f%%\n", "node own work (TOTAL)", double(T) * inv, 100.0);
  printf("  %-32s %11.1f %7.1f%%\n", "pseudo-legal generation", double(st.cPseudoGen) * inv,
         share(st.cPseudoGen, T));
  printf("  %-32s %11.1f %7.1f%%\n", "legality filter", double(legalSum) * inv,
         share(legalSum, T));
  printf("  %-32s %11.1f %7.1f%%\n", "  legality: non-king/ep/castle",
         double(st.cLegalOther) * inv, share(st.cLegalOther, T));
  printf("  %-32s %11.1f %7.1f%%\n", "  legality: king moves", double(st.cLegalKing) * inv,
         share(st.cLegalKing, T));
  printf("  %-32s %11.1f %7.1f%%\n", "  legality: castling", double(st.cLegalCastle) * inv,
         share(st.cLegalCastle, T));
  printf("  %-32s %11.1f %7.1f%%\n", "  legality: en passant", double(st.cLegalEp) * inv,
         share(st.cLegalEp, T));
  printf("  %-32s %11.1f %7.1f%%\n", "do/unmake (incl. check detect)",
         double(st.cDoUndo) * inv, share(st.cDoUndo, T));
  printf("  %-32s %11.1f %7.1f%%\n", "measurement + call overhead",
         double(T - st.cPseudoGen - legalSum - st.cDoUndo) * inv,
         share(T - st.cPseudoGen - legalSum - st.cDoUndo, T));

  const double P = double(st.positions);
  printf("  positions: in-check %.2f%%  double-check %.2f%%  ep %.2f%%  castle-rights %.2f%%\n",
         100.0 * double(st.posInCheck) / P, 100.0 * double(st.posDoubleCheck) / P,
         100.0 * double(st.posWithEp) / P, 100.0 * double(st.posWithCastling) / P);

  const double M = double(st.pseudoMoves);
  printf("  moves/pos: king %.2f  pawn %.2f  promo %.2f  ep %.3f  castle %.2f\n",
         double(st.mvKing) * inv, double(st.mvPawn) * inv, double(st.mvPromotion) * inv,
         double(st.mvEp) * inv, double(st.mvCastle) * inv);
  (void)M;
}

// Isolates the cost of one attackersTo() call, which is the primitive behind
// both check detection and the current legality test. Positions are reached by
// walking the tree so no FEN parsing is included in the timed region.
struct MicroStats {
  unsigned long long calls = 0;
  unsigned long long cycles = 0;
};

void microWalk(Position& pos, int depth, int inner, MicroStats& ms) {
  Move buf[256];
  int n = 0;
  generateLegal(pos, buf, n);
  if (depth <= 0) return;

  for (int i = 0; i < n; ++i) {
    pos.doMove(buf[i]);

    const Square ksq = pos.kingSquare(pos.sideToMove());
    const Bitboard occ = pos.pieces();
    unsigned long long local = 0;
    const unsigned long long t0 = __rdtsc();
    for (int k = 0; k < inner; ++k) local += pos.attackersTo(ksq, occ);
    const unsigned long long t1 = __rdtsc();
    g_sink += local;
    ms.cycles += t1 - t0;
    ms.calls += (unsigned long long)inner;

    microWalk(pos, depth - 1, inner, ms);
    pos.undoMove(buf[i]);
  }
}

void microAttackersTo(const Case& c, int depth, int inner) {
  Position pos;
  std::string err;
  if (!pos.setFromFen(c.fen, &err)) return;
  MicroStats ms;
  microWalk(pos, depth, inner, ms);
  if (ms.calls)
    printf("  %-20s attackersTo %7.1f cycles/call  (%llu calls)\n", c.name,
           double(ms.cycles) / double(ms.calls), ms.calls);
}

// --- Added-work ablation -----------------------------------------------------
//
// Every variant walks the identical tree and performs identical move
// generation, so the node count and traversal cost are the same. They differ
// only by extra per-node work. Timing the difference gives the MARGINAL cost of
// that work with no instrumentation overhead at all, which is what makes these
// numbers trustworthy where the per-move rdtsc numbers were not.

enum class Work {
  Baseline,    // generateLegal + recurse only
  Attackers,   // + N attackersTo() calls per node
  LegalFilter, // + generatePseudoLegal + legal() on each pseudo move
  DoUndoPair,  // + one doMove/undoMove pair per legal move
};

unsigned long long g_nodes = 0;
unsigned long long g_lastNodes = 0;

void walkWork(Position& pos, int depth, Work w, int inner) {
  Move buf[256];
  int n = 0;
  generateLegal(pos, buf, n);
  ++g_nodes;

  switch (w) {
    case Work::Attackers: {
      const Square ksq = pos.kingSquare(pos.sideToMove());
      const Bitboard occ = pos.pieces();
      unsigned long long acc = 0;
      for (int k = 0; k < inner; ++k) acc += pos.attackersTo(ksq, occ);
      g_sink += acc;
      break;
    }
    case Work::LegalFilter: {
      Move pb[256];
      int np = 0;
      generatePseudoLegal(pos, pb, np);
      int nl = 0;
      for (int i = 0; i < np; ++i)
        if (pos.legal(pb[i])) ++nl;
      g_sink += (unsigned long long)nl;
      break;
    }
    case Work::DoUndoPair: {
      for (int i = 0; i < n; ++i) {
        pos.doMove(buf[i]);
        pos.undoMove(buf[i]);
      }
      break;
    }
    case Work::Baseline:
      break;
  }

  if (depth <= 0) return;
  for (int i = 0; i < n; ++i) {
    pos.doMove(buf[i]);
    walkWork(pos, depth - 1, w, inner);
    pos.undoMove(buf[i]);
  }
}

double timeWork(const Case& c, Work w, int inner, int depth) {
  Position pos;
  std::string err;
  if (!pos.setFromFen(c.fen, &err)) return 0.0;
  g_nodes = 0;
  // Warm up so the first measurement does not pay for cold caches.
  walkWork(pos, 2, w, inner);
  g_nodes = 0;
  const auto t0 = std::chrono::steady_clock::now();
  walkWork(pos, depth, w, inner);
  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  if (w == Work::Baseline) g_lastNodes = g_nodes;
  return secs;
}

}  // namespace

int main(int argc, char** argv) {
  int reps = 3;
  int ablDepth = 3;
  int ablInner = 16;
  bool doBench = true, doProfile = false, doMicro = false, doAblate = false, special = false, useRef = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--profile")) { doBench = false; doProfile = true; }
    else if (!std::strcmp(argv[i], "--both")) { doBench = true; doProfile = true; }
    else if (!std::strcmp(argv[i], "--micro")) { doBench = false; doMicro = true; }
    else if (!std::strcmp(argv[i], "--ablate")) { doBench = false; doAblate = true; }
    else if (!std::strcmp(argv[i], "--special")) special = true;
    else if (!std::strcmp(argv[i], "--reference")) { doBench = false; doProfile = true; useRef = true; }
    else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--ablate-depth") && i + 1 < argc)
      ablDepth = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--ablate-inner") && i + 1 < argc)
      ablInner = std::atoi(argv[++i]);
  }

  if (doBench) {
    printf("=====================================================================\n");
    printf("BENCH  perft traversal, %d reps\n", reps);
    printf("=====================================================================\n");
    double sum = 0.0;
    for (const Case& c : kCases) {
      Position pos;
      std::string err;
      if (!pos.setFromFen(c.fen, &err)) {
        printf("%s FEN FAILED: %s\n", c.name, err.c_str());
        return 1;
      }
      printf("\n%s (depth %d)\n", c.name, c.depth);
      std::vector<double> runs;
      for (int r = 0; r < reps; ++r) runs.push_back(runBench(pos, c.depth));
      std::sort(runs.begin(), runs.end());
      sum += runs[runs.size() / 2];
      printf("  median %.0f  (min %.0f  max %.0f)\n", runs[runs.size() / 2], runs.front(),
             runs.back());
    }
    printf("\n  SUM of median nps: %.0f\n", sum);
  }

  if (doProfile) {
    printf("\n=====================================================================\n");
    printf("PROFILE  (rdtsc attribution, node windows exclude children, path=%s)\\n", useRef ? "REFERENCE legal()" : "generateLegal()");
    printf("=====================================================================\n");
    Stats total;
    for (const Case& c : kCases) {
      Position pos;
      std::string err;
      if (!pos.setFromFen(c.fen, &err)) continue;
      Stats st;
      profileNode(pos, c.depth >= 3 ? c.depth - 2 : 1, st, useRef);
      printStats(st, c.name);
      addStats(total, st);
    }
    printStats(total, "ALL STANDARD CASES");

    if (special) {
      Stats sp;
      for (const Case& c : kSpecialCases) {
        Position pos;
        std::string err;
        if (!pos.setFromFen(c.fen, &err)) continue;
        Stats st;
        profileNode(pos, c.depth >= 3 ? c.depth - 2 : 1, st, useRef);
        printStats(st, c.name);
        addStats(sp, st);
      }
      printStats(sp, "ALL SPECIAL CASES");
    }
  }

  if (doMicro) {
    printf("\n=====================================================================\n");
    printf("MICRO  attackersTo cost per call\n");
    printf("=====================================================================\n");
    for (const Case& c : kCases) microAttackersTo(c, 3, 32);
    if (special)
      for (const Case& c : kSpecialCases) microAttackersTo(c, 3, 32);
  }

  if (doAblate) {
    printf("\n=====================================================================\n");
    printf("ABLATION  marginal cost by added work (identical traversal)\n");
    printf("=====================================================================\n");
    printf("  Per node. 'attackersTo xN' charges N attackersTo calls per node,\n");
    printf("  so per-call cost is that column divided by N.\n\n");

    struct Row {
      const char* name;
      double base, atk, leg, dou;
      unsigned long long nodes;
    };
    std::vector<Row> rows;

    for (const Case& c : kCases) {
      // Use the median of several runs to ride out thermal variation.
      const int D = ablDepth;
      auto best = [&](Work w, int inner) {
        std::vector<double> v;
        for (int r = 0; r < reps; ++r) v.push_back(timeWork(c, w, inner, D));
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
      };
      const double b = best(Work::Baseline, 0);
      const unsigned long long nodes = g_lastNodes;
      const double a = best(Work::Attackers, ablInner);
      const double l = best(Work::LegalFilter, 0);
      const double d = best(Work::DoUndoPair, 0);
      rows.push_back({c.name, b, a, l, d, nodes});
    }

    printf("  %-14s %9s %11s %11s %11s %11s\n", "case", "nodes", "baseline", "atk xN", "legalFilter", "doUndoPair");
    double sumBase = 0, sumAtk = 0, sumLeg = 0, sumDou = 0;
    for (const Row& r : rows) {
      const double inv = double(r.nodes);
      printf("  %-14s %9llu %8.1f %11.1f %11.1f %11.1f\n", r.name, r.nodes,
             r.base * 1e9 / inv, (r.atk - r.base) * 1e9 / inv, (r.leg - r.base) * 1e9 / inv,
             (r.dou - r.base) * 1e9 / inv);
      sumBase += r.base;
      sumAtk += (r.atk - r.base);
      sumLeg += (r.leg - r.base);
      sumDou += (r.dou - r.base);
    }
    // Cycle figures assume 4.0 GHz; only the ratios matter.
    const double nodes = double([&] {
      unsigned long long s = 0;
      for (const Row& r : rows) s += r.nodes;
      return s;
    }());
    printf("\n  (cycles/node at an assumed 4.0 GHz -- use ratios, not absolutes)\n");
    printf("  %-34s %9.1f cyc/node  %5.1f%% of baseline\n", "baseline traversal",
           sumBase * 1e9 / nodes, 100.0);
    printf("  %-34s %9.1f cyc/node  %5.1f%%\n", "attackersTo xN per node",
           sumAtk * 1e9 / nodes, 100.0 * sumAtk / sumBase);
    printf("  %-34s %9.1f cyc/node  %5.1f%%\n", "  => per attackersTo call",
           sumAtk * 1e9 / nodes / double(ablInner), 100.0 * sumAtk / sumBase);
    printf("  %-34s %9.1f cyc/node  %5.1f%%\n", "generatePseudoLegal + legal() filter",
           sumLeg * 1e9 / nodes, 100.0 * sumLeg / sumBase);
    printf("  %-34s %9.1f cyc/node  %5.1f%%\n", "one doMove+undoMove pair per legal move",
           sumDou * 1e9 / nodes, 100.0 * sumDou / sumBase);
  }

  return 0;
}
