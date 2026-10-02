// Alpha-beta search.
//
// SCORE CONVENTION
// ----------------
// Every score in this file is from the SIDE TO MOVE's point of view, and stays
// that way at every ply. Negamax negates on the way down, so a node's score is
// directly comparable to evaluate()'s return value, which is also side-to-move
// relative. There is no White-relative value anywhere in the search.
//
// Mixing the two is the classic way to write a search that looks fine and plays
// as if it had the colours reversed, so the convention is asserted in the tests
// rather than left to inspection.
//
// TERMINAL SCORING
// ----------------
// Mate is encoded as a ply count in the mate sentinel range:
//
//     winning:  +kValueMate - ply   -> larger value means a SHORTER mate
//     losing:   -kValueMate + ply   -> larger value means a LONGER loss
//
// Both use "larger is better", so plain max() gives the correct preference in
// both directions with no special cases at the call site.
//
// DRAW MODEL
// ----------
// This engine treats a draw as score 0 in three situations: a position repeated
// within the fifty-move window, a fifty-move counter of 100 or more, and
// insufficient material. Stalemate is also 0. Twofold repetition is used as the
// repetition test, which is stronger than the strict rule; that is the usual
// engine convention on the grounds that the opponent would claim a draw by the
// third occurrence anyway. It is a deliberate, documented choice rather than an
// accident.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "evaluate.hpp"
#include "movegen.hpp"
#include "position.hpp"
#include "tt.hpp"

namespace pocket {

// Kept well below kValueMateInMaxPly so that a real mate score and a ply count
// can never be confused.
constexpr int kMaxSearchPly = 128;

// Winning: a shorter mate scores higher, which plain max() then prefers.
inline int mateInScore(int ply) { return kValueMate - ply; }
// Losing: a longer defence scores higher, so the engine prefers to postpone.
inline int matedInScore(int ply) { return -kValueMate + ply; }

// True when a score is a mate score rather than a centipawn score.
inline bool isMateScore(int s) { return std::abs(s) > kValueMateInMaxPly; }
// Plies to mate implied by a mate score; 0 for a non-mate score.
inline int mateDistanceInPlies(int s) {
  return isMateScore(s) ? kValueMate - std::abs(s) : 0;
}

// --- Move ordering scores ---------------------------------------------------
//
// Ordering is a total order built from disjoint bands, so a category can never
// overtake a strictly more important one no matter how large its tiebreak bonus
// grows. The band constants are powers of two spaced far enough apart that the
// tiebreaks cannot reach the next band.
namespace order {
constexpr int kTTMove = 1 << 30;
constexpr int kGoodCapture = 1 << 26;
constexpr int kPromotion = 1 << 25;
constexpr int kKillerFirst = 1 << 24;
constexpr int kKillerSecond = 1 << 23;
constexpr int kQuietCeiling = (1 << 23) - 1;  // history values live below this
constexpr int kLosingCapture = -(1 << 24);    // sorts after every quiet move
}  // namespace order

// Feature switches, so that each mechanism can be ablated independently.
struct SearchOptions {
  bool useTT = true;
  bool useSeeOrdering = true;
  bool useKillers = true;
  bool useHistory = true;
  bool useQsearch = true;
  // Check extension: force one more ply whenever the side to move is in check.
  // It is switchable because it, and quiescence, both change what a fixed-depth
  // search can see. Tests that compare against the reference need them off so
  // that only the transposition table and ordering are under test.
  bool useCheckExtension = true;
  int ttSizeMB = 16;
};

struct SearchLimits {
  int depth = 0;              // 0 means no depth limit
  uint64_t movetimeMs = 0;    // 0 means no time limit
  uint64_t nodes = 0;         // 0 means no node limit
  // No depth or time limit at all: search until stopped. Only reachable from
  // UCI go infinite / go ponder, which run on a worker thread so that a stop
  // arriving on stdin can interrupt them.
  bool infinite = false;
};

struct SearchStats {
  uint64_t nodes = 0;
  uint64_t qnodes = 0;
  uint64_t ttProbes = 0;
  uint64_t ttHits = 0;
  uint64_t ttCutoffs = 0;
  uint64_t betaCutoffs = 0;
  uint64_t firstMoveCutoffs = 0;
  uint64_t checkExtensions = 0;
};

struct SearchResult {
  Move best = kMoveNone;
  int score = 0;
  int depth = 0;      // last fully completed iteration
  int seldepth = 0;   // deepest ply reached, including quiescence
  bool completed = false;
  std::vector<Move> pv;
  SearchStats stats;
  uint64_t elapsedMs = 0;
};

// Set from outside the UCI loop so `stop` can interrupt a running search. It is
// atomic because with an infinite search the reader thread sets it while the
// search thread is testing it, and a plain bool would be a data race.
extern std::atomic<bool> g_searchStopRequested;

// A draw that follows from the position alone, with no reference to how the
// game reached it.
//
// At the ROOT of a search these two are applied, because they describe the
// position in front of us: a fifty-move counter already at 100 means the game is
// finished, and bare kings cannot be mated. Repetition is deliberately NOT in
// this set. Whether the position in front of the search has already occurred
// depends on game history that the caller owns, and treating an arbitrary root
// as drawn would make the engine refuse to play a position that is perfectly
// playable as a fresh puzzle.
//
// Everywhere else in the tree all three apply, because there a repetition really
// is a draw the side to move can rely on.
inline bool isPositionallyDrawn(const Position& pos) {
  return pos.isFiftyMoveDraw() || pos.hasInsufficientMaterial();
}

class Search {
 public:
  Search(Position& pos, SearchOptions opts = {});

  // Iterative deepening from 1 to limits.depth (or until a limit trips). The
  // result of the last COMPLETED iteration is what comes back: a partially
  // finished iteration must never overwrite a complete earlier one with a
  // half-searched line.
  SearchResult search(const SearchLimits& limits);

  // Fixed-depth entry point with no time management, for tests and benchmarks.
  SearchResult searchDepth(int depth);

  // --- Reference search ----------------------------------------------------
  //
  // A deliberately plain negamax: no transposition table, no move ordering, no
  // quiescence search, no extensions. It exists to be obviously correct so it
  // can act as an oracle for the optimised search at small depths. Any bug
  // shared by both would have to be introduced twice, independently.
  int searchReference(int depth, int alpha = -kValueInfinity, int beta = kValueInfinity);
  std::vector<Move> referencePv(int depth);

  const SearchStats& stats() const { return stats_; }
  TranspositionTable& tt() { return tt_; }

  // Individual term scores, for tests that need to reason about the evaluation
  // without going through search.
  static int staticEval(const Position& pos) { return evaluate(pos); }

 private:
  int negamaxReference(Position& pos, int depth, int alpha, int beta, int ply);

  int qsearch(Position& pos, int alpha, int beta, int ply);
  int negamax(Position& pos, int depth, int alpha, int beta, int ply);

  void scoreMoves(const Position& pos, Move* moves, int count, int* scores,
                  Move ttMove, int ply) const;
  void scoreQuiescenceMoves(const Position& pos, Move* moves, int count, int* scores) const;

  int searchRoot(Position& pos, int depth);
  void extractPv(std::vector<Move>& pv) const;

  void resetSearchState();
  bool timeUp() const;
  void noteKiller(Move m, int ply);
  bool isKiller(Move m, int ply) const;
  void updateHistory(const Position& pos, Move m, int depth);
  int historyValue(const Position& pos, Move m) const;

  Position& root_;
  SearchOptions opts_;
  TranspositionTable tt_;

  // Best move of the root in the iteration currently running. Held separately
  // from the PV table so that an aborted iteration can be discarded without
  // losing the previous iteration's answer.
  Move rootBest_ = kMoveNone;

  SearchStats stats_;
  int seldepth_ = 0;
  bool aborted_ = false;

  // Principal variation, stored as a triangular table rather than re-derived
  // from the transposition table, so it is correct even when the TT is off.
  Move pvTable_[kMaxSearchPly + 1][kMaxSearchPly + 1];
  int pvLength_[kMaxSearchPly + 1];

  Move killers_[kMaxSearchPly + 1][2];

  // [colour][pieceType][destination]
  int history_[2][kNumPieceTypesNB][kNumSquares];

  uint64_t startTimeMs_ = 0;
  uint64_t limitMs_ = 0;
  uint64_t limitNodes_ = 0;
};

}  // namespace pocket
