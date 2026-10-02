#include "tt.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace pocket {

namespace {
constexpr size_t kEntryBytes = sizeof(TTEntry);
constexpr size_t kDefaultMB = 16;
}  // namespace

void TranspositionTable::resize(size_t mb) {
  if (mb == 0) mb = kDefaultMB;
  // Round down to a whole number of entries, and keep at least one slot so that
  // modulo by entryCount is always well defined.
  const size_t count = std::max<size_t>(1, (mb * 1024 * 1024) / kEntryBytes);
  if (count == entries_.size()) return;

  // A resize changes the index mapping, so every stored key becomes meaningless.
  // Start from a clean table rather than leaving entries that would appear to
  // match by accident.
  entries_.assign(count, TTEntry{});
  generation_ = 0;
  resetCounters();
}

void TranspositionTable::clear() {
  // Assignment rather than memset: TTEntry has default member initialisers, so
  // it is not trivially copyable and memset on it is undefined behaviour.
  std::fill(entries_.begin(), entries_.end(), TTEntry{});
  // Advancing the generation ages every existing entry out of the replacement
  // policy's "stale" rule without needing to touch them individually.
  generation_ = uint8_t(generation_ + 1);
  resetCounters();
}

void TranspositionTable::resetCounters() {
  probes_ = hits_ = cutoffs_ = stores_ = collisions_ = 0;
}

bool TranspositionTable::shouldReplace(const TTEntry& existing,
                                       const TTEntry& incoming,
                                       uint8_t generation) const {
  // (a) different position: a hash collision, always take the slot.
  if (existing.key != incoming.key) return true;
  // (b) stale entry from an older generation.
  if (existing.generation != generation) return true;
  // (c) equal or deeper search. The comparison is >= so that an equal-depth
  // store also refreshes the best move; a strict > would freeze the move of
  // whichever result happened to arrive first.
  return int(incoming.depth) >= int(existing.depth);
}

TTProbeResult TranspositionTable::probe(uint64_t key, int ply) const {
  ++probes_;
  if (entries_.empty()) return {};

  const TTEntry& e = entries_[key % entries_.size()];

  // The full 64-bit key is compared. A hash collision reads a different key and
  // reports a miss; it can never yield another position's data.
  if (e.key == 0 || e.key != key) return {};

  ++hits_;

  TTProbeResult r;
  r.hit = true;
  // Undo the ply normalisation that store() applied. Without the caller's ply a
  // mate score would come back with the wrong distance to mate.
  r.score = scoreFromTT(e.score, ply);
  r.depth = e.depth;
  r.bound = BoundType(e.bound);
  r.move = Move(e.move);
  return r;
}

void TranspositionTable::store(const TTEntry& entry, uint8_t generation) {
  if (entries_.empty()) return;
  ++stores_;

  TTEntry& slot = entries_[entry.key % entries_.size()];
  if (slot.key != 0 && slot.key != entry.key) ++collisions_;

  TTEntry incoming = entry;
  incoming.generation = generation;
  // Defensive clamp into the int16 range the entry can hold. Search never
  // produces anything outside it: the largest mate score plus a ply is around
  // 32200, below INT16_MAX. Clamping rather than trusting that is what stops a
  // future caller from silently wrapping a score into a different number.
  incoming.score =
      int16_t(std::max(-32768, std::min(32767, int(incoming.score))));

  if (shouldReplace(slot, incoming, generation)) slot = incoming;
}

int TranspositionTable::hashfull() const {
  if (entries_.empty()) return 0;
  // Sample the first thousand slots. The whole table would be a long scan, and
  // hashfull is only ever read for reporting.
  const size_t sample = std::min<size_t>(1000, entries_.size());
  int used = 0;
  for (size_t i = 0; i < sample; ++i)
    if (entries_[i].key != 0) ++used;
  return int((used * 1000) / int(sample));
}

}  // namespace pocket
