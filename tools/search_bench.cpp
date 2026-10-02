// Search benchmark and ablation harness.
//
// Two jobs, deliberately kept in one binary so that a comparison can never be
// made across binaries built from different source:
//
//   --bench     search a fixed set of positions at fixed depths and report
//                nodes, NPS, quiescence nodes and transposition table statistics
//   --ablate    run the same workload with features switched off one at a time,
//                so the gain from each mechanism is measured rather than assumed
//
// ABLATION PROTOCOL
// -----------------
// Each configuration runs the same positions at the same depths, in the same
// process, and is repeated. The machine throttles under sustained load, which
// Phase 2 already showed can move NPS by 40% across identical runs, so the
// numbers are reported per repetition rather than as one summary figure, and
// node counts are treated as the reliable measure while time is reported for
// context.
//
// Node counts are the primary comparison because they are deterministic. Wall
// clock is secondary and noisy here.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine/evaluate.hpp"
#include "engine/search.hpp"

using namespace pocket;

namespace {

struct BenchPosition {
  const char* fen;
  const char* name;
};

// A spread of opening, middlegame, tactical and endgame positions. Chosen to
// exercise different move types rather than to be famous test sets.
const BenchPosition kPositions[] = {
    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "startpos"},
    {"r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4", "italian"},
    {"r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2N2N2/PPPP1PPP/R1BQK2R w KQkq - 6 5", "two knights"},
    {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", "kiwipete-like"},
    {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", "endgame-rook"},
    {"2r3k1/p4p1p/1p2p1p1/8/2P5/1P3P2/P3B1PP/2R3K1 w - - 0 1", "queenless"},
    {"4k3/8/8/8/8/8/4P3/4K3 w - - 0 1", "kp endgame"},
    {"6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1", "back rank mate available"},
    {"rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq c6 0 2", "sicilian"},
};

constexpr int kNumPositions = int(sizeof(kPositions) / sizeof(kPositions[0]));

uint64_t nowMs() {
  using namespace std::chrono;
  return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

struct Totals {
  uint64_t nodes = 0;
  uint64_t qnodes = 0;
  uint64_t ttProbes = 0;
  uint64_t ttHits = 0;
  uint64_t ttCutoffs = 0;
  uint64_t betaCutoffs = 0;
  uint64_t firstMoveCutoffs = 0;
  uint64_t elapsedMs = 0;
  int positionsOk = 0;
  int positionsIllegal = 0;
};

Totals runOnce(const SearchOptions& opts, int depth) {
  Totals t;
  const uint64_t start = nowMs();
  for (int i = 0; i < kNumPositions; ++i) {
    Position pos;
    std::string err;
    if (!pos.setFromFen(kPositions[i].fen, &err)) {
      printf("FEN PARSE FAILED: %s (%s)\n", err.c_str(), kPositions[i].fen);
      continue;
    }
    Search search(pos, opts);
    const SearchResult r = search.searchDepth(depth);

    // Legality is checked on every single search: an illegal bestmove is the
    // failure that matters, and it is not worth a separate test to find.
    if (r.best == kMoveNone || !pos.legal(r.best))
      ++t.positionsIllegal;
    else
      ++t.positionsOk;

    t.nodes += r.stats.nodes;
    t.qnodes += r.stats.qnodes;
    t.ttProbes += r.stats.ttProbes;
    t.ttHits += r.stats.ttHits;
    t.ttCutoffs += r.stats.ttCutoffs;
    t.betaCutoffs += r.stats.betaCutoffs;
    t.firstMoveCutoffs += r.stats.firstMoveCutoffs;
  }
  t.elapsedMs = nowMs() - start;
  return t;
}

void report(const char* label, const SearchOptions& opts, int depth, int reps) {
  printf("%-34s depth=%d reps=%d\n", label, depth, reps);
  uint64_t bestNodes = ~0ULL;
  for (int r = 0; r < reps; ++r) {
    const Totals t = runOnce(opts, depth);
    bestNodes = std::min(bestNodes, t.nodes);
    const double nps = t.elapsedMs ? double(t.nodes) * 1000.0 / double(t.elapsedMs) : 0;
    const double hitRate = t.ttProbes ? 100.0 * double(t.ttHits) / double(t.ttProbes) : 0;
    printf("  rep%d nodes=%-10llu qnodes=%-10llu nps=%-10.0f time=%-7llu "
           "ttHits=%-8llu ttHitRate=%5.2f%% cutoffs=%llu betaCut=%llu firstMoveCut=%llu\n",
           r, (unsigned long long)t.nodes, (unsigned long long)t.qnodes, nps,
           (unsigned long long)t.elapsedMs, (unsigned long long)t.ttHits, hitRate,
           (unsigned long long)t.ttCutoffs, (unsigned long long)t.betaCutoffs,
           (unsigned long long)t.firstMoveCutoffs);
    if (t.positionsIllegal != 0)
      printf("  *** %d ILLEGAL BESTMOVES ***\n", t.positionsIllegal);
  }
  printf("  best-nodes=%llu\n", (unsigned long long)bestNodes);
}

void doBench(int depth, int reps) {
  printf("=== search benchmark ===\n");
  printf("positions=%d  hardware: AMD Ryzen 7 7435HS\n", kNumPositions);
  printf("node counts are the reliable measure; wall clock on this machine varies "
         "with thermal load\n\n");
  report("full engine", SearchOptions{}, depth, reps);

  // Per-position detail at the requested depth, which is what makes a
  // regression traceable to a single position rather than a total.
  printf("\n--- per position, full engine, depth %d ---\n", depth);
  for (int i = 0; i < kNumPositions; ++i) {
    Position pos;
    std::string err;
    if (!pos.setFromFen(kPositions[i].fen, &err)) continue;
    Search search(pos);
    const SearchResult r = search.searchDepth(depth);
    printf("  %-20s nodes=%-10llu score=%-7d legal=%d\n", kPositions[i].name,
           (unsigned long long)r.stats.nodes, r.score,
           (r.best != kMoveNone && pos.legal(r.best)) ? 1 : 0);
  }
}

void doAblate(int depth, int reps) {
  printf("=== ablation: each mechanism measured separately ===\n");
  printf("depth=%d reps=%d. Compare 'best-nodes' across the rows; a row that\n"
         "searches more nodes than 'full engine' is costing time rather than "
         "saving it.\n\n",
         depth, reps);

  SearchOptions full;
  report("full engine (TT+SEE+killer+hist)", full, depth, reps);

  SearchOptions noTT = full;
  noTT.useTT = false;
  report("no transposition table", noTT, depth, reps);

  SearchOptions noSee = full;
  noSee.useSeeOrdering = false;
  report("no SEE ordering", noSee, depth, reps);

  SearchOptions noKillers = full;
  noKillers.useKillers = false;
  report("no killers", noKillers, depth, reps);

  SearchOptions noHistory = full;
  noHistory.useHistory = false;
  report("no history", noHistory, depth, reps);

  SearchOptions noQsearch = full;
  noQsearch.useQsearch = false;
  report("no quiescence search", noQsearch, depth, reps);

  SearchOptions noExt = full;
  noExt.useCheckExtension = false;
  report("no check extension", noExt, depth, reps);

  // Cumulative build-up, which is how the pieces will actually be adopted.
  printf("\n--- cumulative ---\n");
  SearchOptions bare;
  bare.useTT = false;
  bare.useSeeOrdering = false;
  bare.useKillers = false;
  bare.useHistory = false;
  report("baseline: no TT, no ordering", bare, depth, reps);

  SearchOptions step1 = bare;
  step1.useTT = true;
  report("baseline + TT", step1, depth, reps);

  SearchOptions step2 = step1;
  step2.useSeeOrdering = true;
  report("baseline + TT + SEE ordering", step2, depth, reps);

  SearchOptions step3 = step2;
  step3.useKillers = true;
  report("baseline + TT + SEE + killers", step3, depth, reps);

  SearchOptions step4 = step3;
  step4.useHistory = true;
  report("baseline + TT + SEE + killers + hist", step4, depth, reps);
}

}  // namespace

int main(int argc, char** argv) {
  bool doBenchMode = false, doAblateMode = false;
  int depth = 6, reps = 3;

  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--bench")) doBenchMode = true;
    else if (!std::strcmp(argv[i], "--ablate")) doAblateMode = true;
    else if (!std::strcmp(argv[i], "--depth") && i + 1 < argc) depth = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::atoi(argv[++i]);
  }
  if (!doBenchMode && !doAblateMode) doBenchMode = true;

  if (doBenchMode) doBench(depth, reps);
  if (doAblateMode) doAblate(depth, reps);
  return 0;
}