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
  // Generate only captures and promotions inside quiescence when not in check.
  // Semantically a no-op: quiescence discarded those moves anyway. Present as an
  // option so the saving can be attributed by measurement rather than assumed.
  bool useQsearchCaptureGeneration = true;
  // Principal Variation Search: search the first move with a full window and
  // the rest with a null window, re-searching only when the null-window
  // result beats alpha. Present as an option so the node-count effect can be
  // attributed by measurement rather than assumed.
  bool usePVS = true;
  // Aspiration windows: use the previous iteration's score to narrow the
  // search window, with fail-low/fail-high recovery.
  bool useAspiration = true;
  // Null move pruning: if passing still leaves the position strong enough to
  // beat beta, the opponent cannot save themselves. Conservative baseline:
  // R=2, only when not in check and with non-pawn material on the board.
  bool useNullMove = true;
  // Late Move Reductions: quiet moves searched late in the move loop are
  // unlikely to be best, so search them with reduced depth. A reduced move
  // that still beats alpha is re-searched at full depth with the null
  // window, then with the full window only if the null window also cannot
  // refute it. Excludes captures, promotions, checking moves, TT moves,
  // and killer moves.
  bool useLMR = false;
  // Razoring: at depth 1, if the static evaluation is far below alpha,
  // skip the move search entirely and drop straight to quiescence. The
  // position is so bad that even the best move cannot save it.
  bool useRazoring = true;
  // Reverse futility pruning: at depth <= 2, if the static evaluation
  // minus a margin is still >= beta, the position is so good that even
  // a quiet move that loses the margin cannot drop it below beta.
  bool useRFP = true;
  int ttSizeMB = 16;
};

// Per-move time budget derived from a clock.
//
// The allocation is deliberately simple and fully documented rather than clever,
// because a match has to be reproducible and the arithmetic has to be arguable
// later. With T the side to move's remaining milliseconds, I its increment, and
// M the moves to go:
//
//     movesLeft = movestogo + 40        (40 is the extension into the endgame)
//     soft      = T / movesLeft + 3I/4
//     limit     = min(T * 2/5, soft * 3) - overhead
//
// The 2/5 cap is what stops a large remaining clock from producing a single move
// long enough to blow it. The 3/4 increment fraction means three increments in a
// row can be spent on one move, which is the usual convention.
//
// When no clock is supplied the budget is zero and the search is bounded by depth,
// nodes, or movetime instead.
constexpr int kTimeExtension = 40;      // moves assumed still to come after movestogo
constexpr int kDefaultMovestogo = 30;  // used when movestogo is absent
constexpr uint64_t kProtocolOverheadMs = 10;

struct SearchLimits;

uint64_t computeTimeBudget(const SearchLimits& limits, Color sideToMove,
                           uint64_t overheadMs = kProtocolOverheadMs);

struct SearchLimits {
  int depth = 0;              // 0 means no depth limit
  uint64_t movetimeMs = 0;    // 0 means no time limit
  uint64_t nodes = 0;         // 0 means no node limit
  bool infinite = false;

  // Clock control, set from "go wtime .. btime .. winc .. binc .. movestogo".
  // A value of 0 means "not supplied", which is how a plain "go depth 5" is
  // distinguished from a clock-based game.
  uint64_t wtimeMs = 0;
  uint64_t btimeMs = 0;
  uint64_t wincMs = 0;
  uint64_t bincMs = 0;
  int movestogo = 0;          // 0 means not supplied

  // Filled in from the above, not set directly by callers.
  uint64_t timeLimitMs = 0;

  // True when any clock field was supplied. `go` with no arguments at all is
  // treated as a short fixed-depth search rather than as an infinite clock.
  bool usesClock() const {
    return wtimeMs != 0 || btimeMs != 0 || wincMs != 0 || bincMs != 0 || movestogo != 0;
  }
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
  uint64_t nullMoveAttempts = 0;
  uint64_t nullMoveCutoffs = 0;
  uint64_t lmrReductions = 0;
  uint64_t lmrResearches = 0;

  // Quiescence pipeline decomposition. Counts rather than timings: to turn these
  // into a cost breakdown each operation's standalone cost is measured separately
  // and the product compared against the measured total, which is a checkable
  // estimate rather than a guess. These are diagnostic only and compile out.
  uint64_t qInCheckNodes = 0;      // quiescence nodes that are in check
  uint64_t qEvalCalls = 0;         // stand-pat evaluations
  uint64_t qMovegenCalls = 0;      // generateLegal calls from quiescence
  uint64_t qMovesGenerated = 0;    // moves those calls produced, before filtering
  uint64_t qMovesSearched = 0;     // captures/promotions kept, or evasions
  uint64_t qMakeUnmake = 0;        // doMove/undoMove pairs from quiescence
  uint64_t qSeeCalls = 0;          // seeCapture calls from quiescence ordering
  uint64_t qSelectionSortPasses = 0;  // selection-sort passes while ordering
  uint64_t mainSeeCalls = 0;       // seeCapture calls from main-search ordering
  uint64_t mainMovegenCalls = 0;
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
  Search(Position& pos, SearchOptions opts, TranspositionTable& sharedTT, bool preserveTT = true);

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
                  Move ttMove, int ply);
  void scoreQuiescenceMoves(const Position& pos, Move* moves, int count, int* scores);

  int searchRoot(Position& pos, int depth, int alpha, int beta);
  void extractPv(std::vector<Move>& pv) const;

  void resetSearchState();
  bool timeUp() const;
  void noteKiller(Move m, int ply);
  bool isKiller(Move m, int ply) const;
  void updateHistory(const Position& pos, Move m, int depth);
  int historyValue(const Position& pos, Move m) const;

  // Diagnostic counters. Each is `if constexpr`, so with POCKET_INSTRUMENT at 0
  // the body is discarded at compile time rather than skipped at run time.
  // Defined in the class body so they inline.
  void countQNode() {
    if constexpr (kInstrumented) ++stats_.qnodes;
  }
  void countTtProbe() {
    if constexpr (kInstrumented) ++stats_.ttProbes;
  }
  void countTtHit() {
    if constexpr (kInstrumented) ++stats_.ttHits;
  }
  void countTtCutoff() {
    if constexpr (kInstrumented) ++stats_.ttCutoffs;
  }
  void countBetaCutoff(bool firstMove) {
    if constexpr (kInstrumented) {
      ++stats_.betaCutoffs;
      if (firstMove) ++stats_.firstMoveCutoffs;
    }
  }
  void countCheckExtension() {
    if constexpr (kInstrumented) ++stats_.checkExtensions;
  }
  // Quiescence pipeline counters, diagnostic only. `inQsearch` distinguishes the
  // quiescence copies from the main-search ones so both share these helpers.
  void countQ(bool inQsearch, uint64_t SearchStats::*field) {
    if constexpr (kInstrumented) ++(stats_.*field);
    (void)inQsearch;
  }
  void countQAdd(bool inQsearch, uint64_t SearchStats::*field, uint64_t n) {
    if constexpr (kInstrumented) stats_.*field += n;
    (void)inQsearch;
  }

  Position& root_;
  SearchOptions opts_;
  TranspositionTable internalTt_;
  TranspositionTable& tt_;
  bool preserveTT_ = false;

  // Best move of the root in the iteration currently running. Held separately
  // from the PV table so that an aborted iteration can be discarded without
  // losing the previous iteration's answer.
  Move rootBest_ = kMoveNone;

  SearchStats stats_;
  int seldepth_ = 0;
  bool aborted_ = false;
  bool lastMoveWasNull_ = false;
  int prevScore = kValueNone;  // marks "no previous score" for aspiration

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
