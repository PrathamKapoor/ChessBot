// Transposition table.
//
// A Zobrist-keyed hash table mapping a position to a previously computed score,
// the depth that score was computed at, the bound it satisfies, and the move
// that produced it.
//
// COLLISION STRATEGY
// ------------------
// The table is indexed by the low bits of the 64-bit Zobrist key, but each
// entry stores the ENTIRE 64-bit key and a probe compares all 64 bits:
//
//     entry.key == pos.key()
//
// A hash index collision therefore resolves to a miss followed by an overwrite.
// It can never return another position's score, depth or move. Storing a
// truncated key would save a few bytes and would risk silently returning an
// evaluation for the wrong position, which is the single most dangerous class of
// search bug because it produces plausible-looking scores rather than crashes.
// The full key is stored deliberately.
//
// ENTRY FORMAT
// ------------
// One 16-byte entry, so entries never straddle a cache line in pairs:
//
//     offset  0  uint64  key          full Zobrist key
//     offset  8  uint16  move         best move, 0 means none
//     offset 10  int16   score        already normalised for storage
//     offset 12  int8    depth        depth actually searched
//     offset 13  uint8   bound        BoundType
//     offset 14  uint8   generation   age of the entry
//     offset 15  uint8   pad
//
// depth, bound and generation fit in single bytes because a search depth beyond
// 127 is never useful, and the age wraps at 256 generations.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "types.hpp"

namespace pocket {

// What a stored score tells us about the true value of the position.
enum BoundType : uint8_t {
  kBoundNone = 0,
  kBoundExact = 1,   // score is the true value
  kBoundLower = 2,   // true value >= score
  kBoundUpper = 3,   // true value <= score
};

struct TTEntry {
  uint64_t key = 0;
  uint16_t move = 0;
  int16_t score = 0;
  int8_t depth = 0;
  uint8_t bound = kBoundNone;
  uint8_t generation = 0;
  uint8_t pad = 0;
};
static_assert(sizeof(TTEntry) == 16, "TT entry must stay 16 bytes");

struct TTProbeResult {
  bool hit = false;
  int score = 0;
  int depth = 0;
  BoundType bound = kBoundNone;
  Move move = kMoveNone;
};

// Mate scores are relative to the node they were computed at, so "mate in 2"
// means something different at ply 10 than at ply 0. These two helpers are the
// only place that conversion happens.
//
// A score is "mate-relative" when its magnitude exceeds kValueMateInMaxPly,
// which is the largest mate distance the search can express. Anything smaller is
// an ordinary centipawn score and is stored verbatim.
inline int scoreToTT(int score, int ply) {
  return (std::abs(score) > kValueMateInMaxPly) ? score + ply : score;
}

inline int scoreFromTT(int score, int ply) {
  return (std::abs(score) > kValueMateInMaxPly) ? score - ply : score;
}

class TranspositionTable {
 public:
  TranspositionTable() { resize(0); }

  // Sizes the table to hold roughly `mb` megabytes. Any existing contents are
  // discarded, because a resize changes the index mapping and every stored key
  // would be meaningless. Rounds down to a whole number of entries.
  void resize(size_t mb);
  void clear();

  size_t sizeBytes() const { return entries_.size() * sizeof(TTEntry); }
  size_t entryCount() const { return entries_.size(); }

  // Looks up `key` as seen from `ply`. The ply is needed because a stored mate
  // score is ply-relative and must be shifted back before use.
  //
  // Counter note: this increments a probe counter on every call, which is inside
  // the search's hot path. That cost is measured by comparing runs with
  // statistics on and off rather than assumed to be free.
  TTProbeResult probe(uint64_t key, int ply) const;

  // Stores `entry`, applying the replacement policy. The score stored here must
  // already have been through scoreToTT().
  void store(const TTEntry& entry, uint8_t generation);

  // Per-mille of the first 1000 slots that hold an entry. Used to report how
  // full the table is and to choose a size.
  int hashfull() const;

  // --- Statistics ----------------------------------------------------------
  //
  // These are plain counters rather than anything clever. Their cost is measured
  // separately in the benchmarks rather than assumed to be free; see RESEARCH.md.
  void resetCounters();
  uint64_t probes() const { return probes_; }
  uint64_t hits() const { return hits_; }
  uint64_t cutoffs() const { return cutoffs_; }
  uint64_t stores() const { return stores_; }
  uint64_t collisions() const { return collisions_; }

  uint8_t generation() const { return generation_; }

 private:
  // REPLACEMENT POLICY
  //
  // One policy, not a configurable family. An entry is overwritten when:
  //
  //   (a) the slot holds a different position, i.e. a hash collision, or
  //   (b) the entry is from an older generation, i.e. it is stale, or
  //   (c) the incoming search reached at least the same depth.
  //
  // Rule (c) is what makes the table usable across iterative deepening: a
  // depth-2 result from the previous iteration must not evict a depth-9 result
  // from this one.
  //
  // The comparison is >= rather than >. Requiring strictly greater depth would
  // mean an equal-depth store is rejected, and with it the best move, so a
  // position searched twice at the same depth would keep reporting the first
  // move it found forever. Refreshing on a tie costs nothing and keeps the
  // stored move current.
  bool shouldReplace(const TTEntry& existing, const TTEntry& incoming,
                     uint8_t generation) const;

  std::vector<TTEntry> entries_;
  uint8_t generation_ = 0;

  // Mutable so that probe() can stay logically const while still recording
  // statistics.
  mutable uint64_t probes_ = 0, hits_ = 0, cutoffs_ = 0;
  uint64_t stores_ = 0, collisions_ = 0;
};

}  // namespace pocket
