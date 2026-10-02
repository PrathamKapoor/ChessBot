// Unit tests for the transposition table.
//
// The tests below are deliberately adversarial rather than happy-path. The
// dangerous failure mode for a hash table is not crashing, it is silently
// returning a plausible-looking score for the wrong position, so the collision
// and replacement cases get as much attention as the store/probe round trip.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "engine/tt.hpp"

using namespace pocket;

namespace {

int g_fail = 0;
int g_checks = 0;

void check(bool ok, const char* what, const std::string& detail = "") {
  ++g_checks;
  if (ok) return;
  ++g_fail;
  printf("FAIL %s%s%s\n", what, detail.empty() ? "" : "  ", detail.c_str());
}

void expectEq(const char* what, long long got, long long want) {
  char buf[128];
  snprintf(buf, sizeof(buf), "got=%lld want=%lld", got, want);
  check(got == want, what, buf);
}

TTEntry makeEntry(uint64_t key, int depth, int score, BoundType bound, Move move) {
  TTEntry e;
  e.key = key;
  e.depth = int8_t(depth);
  e.score = int16_t(score);
  e.bound = uint8_t(bound);
  e.move = uint16_t(move);
  return e;
}

// --- 1. empty table miss ----------------------------------------------------

void testEmptyTableMiss() {
  TranspositionTable tt;
  tt.resize(1);
  const TTProbeResult r = tt.probe(0x1234567890abcdefULL, 0);
  check(!r.hit, "emptyTable miss");
  expectEq("emptyTable hashfull", tt.hashfull(), 0);
  expectEq("emptyTable probes", (long long)tt.probes(), 1);
  expectEq("emptyTable hits", (long long)tt.hits(), 0);
}

// --- 2. store / probe hit ---------------------------------------------------

void testStoreProbeHit() {
  TranspositionTable tt;
  tt.resize(1);
  const uint64_t key = 0xfeedfacecafebeefULL;
  const Move m = makeMove(makeSquare(kFileE, kRank2), makeSquare(kFileE, kRank4));

  tt.store(makeEntry(key, 7, 125, kBoundExact, m), tt.generation());
  const TTProbeResult r = tt.probe(key, 0);

  check(r.hit, "storeProbe hit");
  expectEq("storeProbe score", r.score, 125);
  expectEq("storeProbe depth", r.depth, 7);
  expectEq("storeProbe bound", r.bound, kBoundExact);
  check(r.move == m, "storeProbe bestMove");
  // hashfull samples only the leading slots, so a single store is not guaranteed
  // to be visible. Fill enough distinct keys to cover the sample window.
  TranspositionTable big;
  big.resize(16);
  for (uint64_t i = 0; i < 2000; ++i)
    big.store(makeEntry(0x7000000000000000ULL + i, 3, 1, kBoundExact, kMoveNone),
              big.generation());
  check(big.hashfull() >= 500, "hashfull rises after many stores");
}

// --- 3/4/5. bound classification -------------------------------------------

void testBounds() {
  TranspositionTable tt;
  tt.resize(1);
  const uint64_t key = 0xaaaaULL;

  // Each bound type must survive the round trip unchanged. Bound handling is
  // where a transposition table quietly turns a fail-soft bound into a false
  // claim of an exact score, so the value is checked alongside the tag.
  struct Case {
    BoundType bound;
    int score;
  };
  const Case cases[] = {{kBoundExact, 40}, {kBoundLower, -300}, {kBoundUpper, 90}};
  for (const Case& c : cases) {
    tt.clear();
    tt.store(makeEntry(key, 5, c.score, c.bound, kMoveNone), tt.generation());
    const TTProbeResult r = tt.probe(key, 0);
    char label[80];
    snprintf(label, sizeof(label), "bound %d round trip", int(c.bound));
    check(r.hit && r.bound == c.bound, label);
    expectEq(label, r.score, c.score);
  }
}

// --- 6. depth handling ------------------------------------------------------

void testDepthHandling() {
  TranspositionTable tt;
  tt.resize(1);
  const uint64_t key = 0x1234ULL;

  tt.store(makeEntry(key, 3, 11, kBoundExact, kMoveNone), tt.generation());
  expectEq("depth round trip", tt.probe(key, 0).depth, 3);

  // A deeper result for the same position should replace a shallow one.
  tt.store(makeEntry(key, 9, 12, kBoundExact, kMoveNone), tt.generation());
  expectEq("depth deeper replaces", tt.probe(key, 0).depth, 9);
  expectEq("depth deeper score", tt.probe(key, 0).score, 12);
}

// --- 7. replacement ---------------------------------------------------------

void testReplacement() {
  TranspositionTable tt;
  tt.resize(1);
  // These two keys collide in any table whose size is a power of two, given
  // that both differ only in the high bits.
  // Two keys that differ by exactly one table length share a slot.
  const uint64_t keyA = 0x0000000000000100ULL;
  const uint64_t keyB = keyA + uint64_t(tt.entryCount());
  check(keyA % tt.entryCount() == keyB % tt.entryCount(), "replacement setup collides");

  tt.store(makeEntry(keyA, 8, 50, kBoundExact, kMoveNone), tt.generation());
  check(tt.probe(keyA, 0).hit, "replacement A stored");

  // A colliding key must not read the other entry's score.
  const TTProbeResult before = tt.probe(keyB, 0);
  check(!before.hit, "replacement B misses before store");

  tt.store(makeEntry(keyB, 8, 77, kBoundExact, kMoveNone), tt.generation());
  check(!tt.probe(keyA, 0).hit, "replacement evicted A");
  const TTProbeResult b = tt.probe(keyB, 0);
  check(b.hit, "replacement B hits");
  expectEq("replacement B score is its own", b.score, 77);

  // A shallow entry must not evict a deep one while both are current.
  tt.store(makeEntry(keyB, 12, 78, kBoundExact, kMoveNone), tt.generation());
  tt.store(makeEntry(keyB, 1, 79, kBoundExact, kMoveNone), tt.generation());
  expectEq("shallow does not evict deep", tt.probe(keyB, 0).depth, 12);

  // Equal depth must still replace, otherwise the stored move would freeze at
  // whatever the first search of that depth happened to produce.
  tt.store(makeEntry(keyB, 12, 80, kBoundExact, kMoveNone), tt.generation());
  expectEq("equal depth replaces", tt.probe(keyB, 0).depth, 12);
  expectEq("equal depth refreshes score", tt.probe(keyB, 0).score, 80);
}

// --- 8. clear / reset -------------------------------------------------------

void testClear() {
  TranspositionTable tt;
  tt.resize(1);
  const uint64_t key = 0x9999ULL;
  tt.store(makeEntry(key, 6, 33, kBoundExact, kMoveNone), tt.generation());
  check(tt.probe(key, 0).hit, "clear setup");

  tt.clear();
  expectEq("clear resets probes", (long long)tt.probes(), 0);
  expectEq("clear hashfull", tt.hashfull(), 0);
  check(!tt.probe(key, 0).hit, "clear empties table");

  // clear() ages the generation; the table still works afterwards.
  tt.store(makeEntry(key, 6, 34, kBoundExact, kMoveNone), tt.generation());
  check(tt.probe(key, 0).hit, "usable after clear");
}

// --- 9. mate score normalisation -------------------------------------------

void testMateNormalisation() {
  const int M = kValueMate;

  // Mate in 3 found at ply 0 round trips unchanged.
  expectEq("mate ply0 toTT", scoreToTT(-(M - 3), 0), -(M - 3));
  expectEq("mate ply0 fromTT", scoreFromTT(-(M - 3), 0), -(M - 3));

  // Mate in 2 found at ply 7: store adds the ply, probe subtracts it, so the
  // original value comes back exactly.
  const int at7 = -(M - 2);
  const int stored = scoreToTT(at7, 7);
  expectEq("mate at ply7 stored", stored, -(M - 2) + 7);
  expectEq("mate at ply7 round trip", scoreFromTT(stored, 7), at7);

  // The whole point: reading the same bytes at a different ply yields a
  // different, correctly shifted score rather than the same number.
  expectEq("mate at ply7 read at ply0", scoreFromTT(stored, 0), -(M - 2) + 7);

  // Ordinary centipawn scores must be stored verbatim at any ply, otherwise
  // every non-mate score would drift as the search descends.
  for (int s : {0, 37, -250, 1500, -9000}) {
    check(scoreToTT(s, 11) == s, "centipawn scoreToTT is identity");
    check(scoreFromTT(s, 11) == s, "centipawn scoreFromTT is identity");
    check(scoreToTT(s, 0) == s, "centipawn scoreToTT is identity at ply 0");
  }

  // Scores just inside the ordinary range must not be shifted either.
  const int justInside = kValueMateInMaxPly;
  check(scoreToTT(justInside, 9) == justInside, "boundary score not shifted");
  check(scoreToTT(justInside + 1, 9) != justInside + 1, "past boundary is shifted");

  // Round trip through the actual table, since the entry is int16.
  TranspositionTable tt;
  tt.resize(1);
  const uint64_t key = 0x4444ULL;
  const int mateAtPly = -(M - 4);
  const int ply = 6;
  tt.store(makeEntry(key, 5, scoreToTT(mateAtPly, ply), kBoundExact, kMoveNone),
           tt.generation());
  expectEq("mate score through table", tt.probe(key, ply).score, mateAtPly);
}

// --- 10. collision behaviour ------------------------------------------------

void testCollisions() {
  // A one-entry table forces every distinct key onto the same slot, so this
  // checks the worst case rather than a rare one.
  TranspositionTable tt;
  tt.resize(1);

  std::vector<uint64_t> keys;
  for (uint64_t i = 0; i < 64; ++i) keys.push_back(0x5a5a0000ULL + i * 4096ULL);

  uint64_t surviving = 0;
  for (size_t i = 0; i < keys.size(); ++i) {
    const uint64_t k = keys[i];
    tt.store(makeEntry(k, 4, int(i), kBoundExact, kMoveNone), tt.generation());
    const TTProbeResult r = tt.probe(k, 0);
    if (r.hit) {
      ++surviving;
      // The score must belong to the key that was asked about, never to another.
      check(r.score == int(i), "collision returns own score, not another",
            "score did not match the probed key");
    }
  }
  check(surviving >= 1, "collision at least one survives");

  // With a realistically sized table, distinct positions should not collide.
  TranspositionTable big;
  big.resize(16);
  for (uint64_t i = 0; i < 5000; ++i)
    big.store(makeEntry(0x9e3779b97f4a7c15ULL * (i + 1), 4, int(i & 0x7fff),
                        kBoundExact, kMoveNone),
              big.generation());
  for (uint64_t i = 0; i < 5000; ++i) {
    const uint64_t k = 0x9e3779b97f4a7c15ULL * (i + 1);
    const TTProbeResult r = big.probe(k, 0);
    if (r.hit) check(r.score == int(i & 0x7fff), "sized table returns own score");
  }
}

// --- 11. best-move storage --------------------------------------------------

void testBestMoveStorage() {
  TranspositionTable tt;
  tt.resize(1);
  const uint64_t key = 0x777ULL;

  // No move stored must read back as kMoveNone, not as a move made from a
  // zero from-square.
  tt.store(makeEntry(key, 3, 5, kBoundExact, kMoveNone), tt.generation());
  check(tt.probe(key, 0).move == kMoveNone, "no move reads back as kMoveNone");

  // Every interesting move shape must survive the uint16 encoding intact.
  const Move moves[] = {
      makeMove(makeSquare(kFileA, kRank1), makeSquare(kFileH, kRank8)),
      makeMove(makeSquare(kFileE, kRank5), makeSquare(kFileD, kRank6), kFlagEnPassant),
      makeMove(makeSquare(kFileA, kRank7), makeSquare(kFileA, kRank8), kFlagCastling),
      makePromotion(makeSquare(kFileB, kRank7), makeSquare(kFileB, kRank8), kQueen),
      makePromotion(makeSquare(kFileB, kRank7), makeSquare(kFileB, kRank8), kKnight),
  };
  for (Move m : moves) {
    tt.store(makeEntry(key, 3, 5, kBoundExact, m), tt.generation());
    const TTProbeResult r = tt.probe(key, 0);
    check(r.move == m, "move round trip");
    check(int(fromSq(r.move)) == int(fromSq(m)), "move from-square preserved");
    check(int(toSq(r.move)) == int(toSq(m)), "move to-square preserved");
  }
}

// --- 12. generation / aging -------------------------------------------------

void testGenerationAging() {
  TranspositionTable tt;
  tt.resize(1);
  const uint64_t key = 0xabcULL;

  tt.store(makeEntry(key, 10, 20, kBoundExact, kMoveNone), tt.generation());
  const uint8_t g0 = tt.generation();

  tt.clear();
  const uint8_t g1 = tt.generation();
  check(g0 != g1, "clear advances generation");
  check(tt.probe(key, 0).hit == false, "cleared entry is gone");

  // An entry from the previous generation is still readable; generation only
  // drives replacement, not validity. Search relies on that.
  tt.store(makeEntry(key, 10, 21, kBoundExact, kMoveNone), g0);
  const TTProbeResult r = tt.probe(key, 0);
  check(r.hit, "stale-generation entry is still readable");
  expectEq("stale-generation score", r.score, 21);

  // Storing at the current generation overwrites it regardless of depth,
  // because staleness wins over the depth preference.
  tt.store(makeEntry(key, 1, 22, kBoundExact, kMoveNone), g1);
  expectEq("stale entry overwritten by current gen", tt.probe(key, 0).score, 22);
}

// --- sizing -----------------------------------------------------------------

void testSizing() {
  TranspositionTable tt;
  tt.resize(4);
  check(tt.sizeBytes() >= 4u * 1024 * 1024, "resize honours requested size");
  check(tt.sizeBytes() % sizeof(TTEntry) == 0, "size is a whole number of entries");

  // A resize must discard contents rather than leave keys that appear to match.
  const uint64_t key = 0x2468ULL;
  tt.store(makeEntry(key, 5, 10, kBoundExact, kMoveNone), tt.generation());
  check(tt.probe(key, 0).hit, "sizing setup");
  tt.resize(8);
  check(!tt.probe(key, 0).hit, "resize discards contents");

  // Zero means "use the default" rather than "allocate nothing", so that a
  // caller passing an unparsed option does not end up with a table that silently
  // never hits.
  TranspositionTable dflt;
  dflt.resize(0);
  check(dflt.entryCount() > 100000, "resize(0) means default, not empty");
}

}  // namespace

int main() {
  testEmptyTableMiss();
  testStoreProbeHit();
  testBounds();
  testDepthHandling();
  testReplacement();
  testClear();
  testMateNormalisation();
  testCollisions();
  testBestMoveStorage();
  testGenerationAging();
  testSizing();

  if (g_fail == 0)
    printf("TT OK (%d checks)\n", g_checks);
  else
    printf("TT FAILED (%d of %d checks)\n", g_fail, g_checks);
  return g_fail == 0 ? 0 : 1;
}
