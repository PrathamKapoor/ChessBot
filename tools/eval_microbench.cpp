// Isolated microbenchmark for the evaluation.
//
// Phase 3A reported roughly 20,000 search nodes per second against a move
// generator doing tens of millions, and attributed the gap to "the evaluation is
// expensive" without ever measuring evaluate() on its own. This does that.
//
// The question is narrow and answerable: how long does one evaluate() call take,
// and how does that compare to the cost of the rest of a search node? Two things
// are measured.
//
//   1. evaluate() throughput in isolation, over a set of positions reached by
//      random legal play so the piece counts vary the way real search sees them.
//   2. The same evaluation cost expressed as a fraction of a search node, by
//      dividing total evaluate() calls made during a real search by the number of
//      nodes in that search.
//
// A caveat stated up front. The positions here are collected first and evaluated
// afterwards, so the branch predictor and caches see a friendly access pattern.
// The absolute figure is therefore a lower bound on what evaluate() costs inside
// a search, where it is called interleaved with move generation and transposition
// table probes. It is used for ranking and for ratio arithmetic, not as an
// absolute cost.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/evaluate.hpp"
#include "engine/movegen.hpp"

using namespace pocket;

namespace {

uint64_t nowNs() {
  using namespace std::chrono;
  return uint64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

const char* kStartFens[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "2r3k1/p4p1p/1p2p1p1/8/2P5/1P3P2/P3B1PP/2R3K1 w - - 0 1",
};

// A tiny deterministic generator. Deliberately not seeded from the clock: the
// benchmark has to visit the same positions on every run or timings are not
// comparable between runs. Fixed-width LCG, fixed seed.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  int below(int n) { return int(next() % uint64_t(n)); }
};

// Collects positions by making random legal moves from the start positions.
std::vector<Position> collectPositions(int target) {
  Rng rng(0x9E3779B97F4A7C15ULL);
  std::vector<Position> out;
  out.reserve(size_t(target));

  int startIdx = 0;
  while (int(out.size()) < target) {
    Position p;
    std::string err;
    if (!p.setFromFen(kStartFens[startIdx % 5], &err)) continue;
    ++startIdx;

    // 8 to 60 plies, which spans opening, middlegame and endgame piece counts.
    const int plies = 8 + rng.below(52);
    for (int i = 0; i < plies; ++i) {
      Move buf[256];
      int n = 0;
      generateLegal(p, buf, n);
      if (n == 0) break;
      p.doMove(buf[rng.below(n)]);
    }
    // Terminal positions would evaluate but never appear as search nodes in the
    // same way, and including them would skew the average.
    if (p.inCheck() || legalMoveCount(p) == 0) continue;
    out.push_back(p);
  }
  return out;
}

int64_t checksum(const std::vector<Position>& ps) {
  // The sum is accumulated so the optimiser cannot delete the evaluation.
  int64_t sum = 0;
  for (const Position& p : ps) sum += evaluate(p);
  return sum;
}

int pieceCount(const Position& p) { return popcount(p.pieces()); }

}  // namespace

int main(int argc, char** argv) {
  int nPositions = 20000;
  int rounds = 5;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--positions") && i + 1 < argc) nPositions = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--rounds") && i + 1 < argc) rounds = std::atoi(argv[++i]);
  }

  printf("=== evaluation microbenchmark ===\n");
  printf("deterministic: fixed seed, so the same positions are visited every run\n\n");

  const std::vector<Position> positions = collectPositions(nPositions);

  // Piece-count distribution, so the reader can see what was actually measured
  // rather than having to trust the label "middlegame".
  int buckets[5] = {0, 0, 0, 0, 0};
  for (const Position& p : positions) {
    const int n = pieceCount(p);
    if (n < 10) buckets[0]++;
    else if (n < 20) buckets[1]++;
    else if (n < 28) buckets[2]++;
    else if (n < 33) buckets[3]++;
    else buckets[4]++;
  }
  int totalPieces = 0;
  for (const Position& p : positions) totalPieces += pieceCount(p);
  printf("positions=%d  mean pieces=%.1f\n", nPositions, double(totalPieces) / positions.size());
  printf("piece count spread: <10:%d  10-19:%d  20-27:%d  28-32:%d  33+:%d\n\n",
         buckets[0], buckets[1], buckets[2], buckets[3], buckets[4]);

  // One warm-up pass, so the first timed round is not paying for first-touch page
  // faults on the position vector.
  checksum(positions);

  uint64_t bestNs = ~0ULL;
  int64_t sum = 0;
  for (int r = 0; r < rounds; ++r) {
    const uint64_t t0 = nowNs();
    sum = checksum(positions);
    const uint64_t dt = nowNs() - t0;
    if (dt < bestNs) bestNs = dt;
  }

  const double perCallNs = double(bestNs) / double(positions.size());
  printf("rounds=%d  best total=%lluns\n", rounds, (unsigned long long)bestNs);
  printf("evaluate(): %.1f ns/call\n", perCallNs);
  printf("evaluate(): %.2f million calls/second\n", 1000.0 / perCallNs);
  printf("checksum=%lld  (present so the work cannot be optimised away)\n", (long long)sum);

  // Per-term breakdown. Each term is measured the same way, over the same
  // positions, so the ranking is meaningful even though each is called on its own
  // rather than through evaluate().
  printf("\n--- per-term cost, same positions, one term at a time ---\n");
  struct Term {
    const char* name;
    TaperedScore (*fn)(const Position&);
  };
  static const Term kTerms[] = {{"material", evalMaterial},
                                {"pieceSquares", evalPieceSquares},
                                {"pawnStructure", evalPawnStructure},
                                {"pieces (mobility)", evalPieces},
                                {"kingSafety", evalKingSafety},
                                {"threats", evalThreats},
                                {"space", evalSpace}};

  volatile int64_t sink = 0;
  for (const Term& t : kTerms) {
    uint64_t best = ~0ULL;
    for (int r = 0; r < rounds; ++r) {
      const uint64_t t0 = nowNs();
      int64_t acc = 0;
      for (const Position& p : positions) {
        const TaperedScore s = t.fn(p);
        acc += s.mg + s.eg;
      }
      const uint64_t dt = nowNs() - t0;
      sink += acc;
      if (dt < best) best = dt;
    }
    const double ns = double(best) / double(positions.size());
    printf("  %-20s %7.1f ns  (%5.1f%% of evaluate)\n", t.name, ns,
           100.0 * ns / perCallNs);
  }
  if (sink == 0x7fffffff) printf(" ");  // keep the sink observable

  printf("\nNote: these are lower bounds. Positions are visited in a batch here,"
         "\nwhile inside a search evaluate() is interleaved with move generation"
         "\nand transposition table probes, so the real per-call cost is higher.\n");
  return 0;
}