#include "search.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace pocket {

std::atomic<bool> g_searchStopRequested{false};

uint64_t computeTimeBudget(const SearchLimits& limits, Color sideToMove,
                           uint64_t overheadMs) {
  const bool white = sideToMove == kWhite;
  const uint64_t time = white ? limits.wtimeMs : limits.btimeMs;
  const uint64_t inc = white ? limits.wincMs : limits.bincMs;
  if (time == 0) return 0;

  const int mtg = limits.movestogo > 0 ? limits.movestogo : kDefaultMovestogo;
  const int movesLeft = mtg + kTimeExtension;
  const uint64_t soft = time / uint64_t(movesLeft) + inc * 3 / 4;
  const uint64_t hard = time * 2 / 5;

  uint64_t budget = soft * 3;
  if (hard < budget) budget = hard;
  if (budget <= overheadMs) return 0;  // too little time to search anything
  return budget - overheadMs;
}

namespace {

uint64_t nowMs() {
  using namespace std::chrono;
  return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Maximum value the history table may reach. History is a heuristic, so letting
// it grow without bound would let early searches permanently dominate ordering
// in a later search, and would eventually overflow the int.
constexpr int kHistoryMax = 16384;
// How fast a bad entry decays. The multiply-and-divide form keeps the value
// inside range without a branch.
constexpr int kHistoryGravity = 160;

}  // namespace

Search::Search(Position& pos, SearchOptions opts) : root_(pos), opts_(opts) {
  if (opts_.ttSizeMB > 0) tt_.resize(opts_.ttSizeMB);
  resetSearchState();
  std::memset(killers_, 0, sizeof(killers_));
  std::memset(history_, 0, sizeof(history_));
  std::memset(pvTable_, 0, sizeof(pvTable_));
}

void Search::resetSearchState() {
  stats_ = SearchStats{};
  seldepth_ = 0;
  aborted_ = false;
  std::memset(pvLength_, 0, sizeof(pvLength_));
  if (opts_.useTT) {
    tt_.clear();
    tt_.resetCounters();
  }
}

bool Search::timeUp() const {
  // Only the two external limits are checked here; depth and node limits are
  // handled by the callers that know when they have been reached.
  if (g_searchStopRequested) return true;
  if (limitMs_ != 0 && nowMs() - startTimeMs_ >= limitMs_) return true;
  if (limitNodes_ != 0 && stats_.nodes >= limitNodes_) return true;
  return false;
}

// --- Reference search -------------------------------------------------------

int Search::searchReference(int depth, int alpha, int beta) {
  return negamaxReference(root_, depth, alpha, beta, /*ply=*/0);
}

int Search::negamaxReference(Position& pos, int depth, int alpha, int beta, int ply) {
  ++stats_.nodes;
  if (ply > seldepth_) seldepth_ = ply;

  // A draw scores zero. At the root only position-local rules apply; see
  // isPositionallyDrawn() for why repetition is excluded there.
  if (ply > 0 ? pos.isDrawByRule() : isPositionallyDrawn(pos)) return 0;

  // Depth zero evaluates. There is no quiescence search here on purpose: the
  // reference must be the simplest thing that could work.
  if (depth <= 0) return evaluate(pos);

  Move moves[256];
  int count = 0;
  generateLegal(pos, moves, count);

  // No legal moves is either checkmate or stalemate, and the two are told apart
  // by whether the side to move is in check.
  if (count == 0) return pos.inCheck() ? matedInScore(ply) : 0;

  int best = -kValueInfinity;
  for (int i = 0; i < count; ++i) {
    pos.doMove(moves[i]);
    const int score = -negamaxReference(pos, depth - 1, -beta, -alpha, ply + 1);
    pos.undoMove(moves[i]);

    // Fail-soft: the best score seen so far is returned, not beta. A cutoff
    // returns a bound rather than an exact value, which is what lets the caller
    // distinguish the two when storing to the transposition table.
    if (score > best) best = score;
    if (score > alpha) alpha = score;
    if (alpha >= beta) break;
  }
  return best;
}

std::vector<Move> Search::referencePv(int depth) {
  // The reference search has no PV machinery, so the line is recovered by
  // re-searching each time, which is only ever done at test-scale depths.
  std::vector<Move> pv;
  Position& pos = root_;
  for (int i = 0; i < depth; ++i) {
    Move moves[256];
    int count = 0;
    generateLegal(pos, moves, count);
    if (count == 0) break;

    int bestScore = -kValueInfinity;
    Move bestMove = kMoveNone;
    for (int m = 0; m < count; ++m) {
      pos.doMove(moves[m]);
      const int score = -negamaxReference(pos, depth - 1 - i, -kValueInfinity,
                                          kValueInfinity, i + 1);
      pos.undoMove(moves[m]);
      if (score > bestScore) {
        bestScore = score;
        bestMove = moves[m];
      }
    }
    if (bestMove == kMoveNone) break;
    pv.push_back(bestMove);
    pos.doMove(bestMove);
  }
  return pv;
}

// --- Move ordering ---------------------------------------------------------

bool Search::isKiller(Move m, int ply) const {
  if (!opts_.useKillers) return false;
  return killers_[ply][0] == m || killers_[ply][1] == m;
}

void Search::noteKiller(Move m, int ply) {
  if (!opts_.useKillers) return;
  // Shift rather than append, so the most recent two cutoffs survive.
  if (killers_[ply][0] != m) {
    killers_[ply][1] = killers_[ply][0];
    killers_[ply][0] = m;
  }
}

int Search::historyValue(const Position& pos, Move m) const {
  if (!opts_.useHistory) return 0;
  const Piece pc = pos.pieceOn(fromSq(m));
  if (pc == kNoPiece) return 0;
  return history_[int(colorOf(pc))][int(typeOf(pc))][int(toSq(m))];
}

void Search::updateHistory(const Position& pos, Move m, int depth) {
  if (!opts_.useHistory) return;
  const Piece pc = pos.pieceOn(fromSq(m));
  if (pc == kNoPiece) return;
  int& h = history_[int(colorOf(pc))][int(typeOf(pc))][int(toSq(m))];
  // Reward quadratically in depth: a quiet move that only works deep is more
  // useful than the same move discovered at depth one.
  h += depth * depth;
  // Gravity pulls old entries down, and the clamp stops the table growing
  // without bound across a whole game.
  h -= h * kHistoryGravity / 256;
  if (h > kHistoryMax) h = kHistoryMax;
  if (h < -kHistoryMax) h = -kHistoryMax;
}

void Search::scoreMoves(const Position& pos, Move* moves, int count, int* scores,
                        Move ttMove, int ply) {
  for (int i = 0; i < count; ++i) {
    const Move m = moves[i];
    const Square to = toSq(m);
    const Square from = fromSq(m);

    // 1. The transposition table move, which is the most likely to produce a
    //    cutoff because a previous search already proved something with it.
    if (m == ttMove) {
      scores[i] = order::kTTMove;
      continue;
    }

    const bool isCapture = pos.captureOrPromotion(m);
    const int victimValue = isCapture ? pieceValue(typeOf(pos.pieceOn(to))) : 0;

    if (isCapture) {
      // Most valuable victim, least valuable attacker. A promotion that
      // captures counts as the promoted piece, not the pawn that moves.
      const Piece mover = pos.pieceOn(from);
      int attackerValue = isPromotion(m) ? pieceValue(promotionType(m))
                                         : pieceValue(typeOf(mover));
      const int mvvlva = victimValue * 16 - attackerValue;

      if (opts_.useSeeOrdering) {
        // SEE decides whether the capture actually wins material. A capture that
        // loses an exchange is worth much less than MVV-LVA alone suggests, and
        // search order that ignores this wastes its budget early.
        countQ(false, &SearchStats::mainSeeCalls);
        const int see = seeCapture(pos, m);
        if (see >= 0) {
          // Small SEE bonus breaks ties among equal MVV-LVA captures, which
          // keeps the ordering informative without letting it dominate the band.
          scores[i] = order::kGoodCapture + mvvlva + (see < 64 ? see * 8 : 512);
        } else {
          // Losing captures are searched last: they are legal but usually a
          // mistake, so spending the good part of the budget elsewhere pays.
          scores[i] = order::kLosingCapture + mvvlva;
        }
      } else {
        scores[i] = order::kGoodCapture + mvvlva;
      }
      continue;
    }

    // 3. Promotions that are not captures.
    if (isPromotion(m)) {
      scores[i] = order::kPromotion + pieceValue(promotionType(m));
      continue;
    }

    // 4. Killer moves: quiet moves that already produced a cutoff at this ply in
    //    a sibling branch.
    if (isKiller(m, ply)) {
      scores[i] = (killers_[ply][0] == m) ? order::kKillerFirst : order::kKillerSecond;
      continue;
    }

    // 5. History, then 6. plain quiet moves at zero.
    scores[i] = historyValue(pos, m) + 1;
  }
}

void Search::scoreQuiescenceMoves(const Position& pos, Move* moves, int count,
                                  int* scores) {
  for (int i = 0; i < count; ++i) {
    const Move m = moves[i];
    if (!pos.captureOrPromotion(m)) {
      scores[i] = 0;
      continue;
    }
    const int victimValue = pieceValue(typeOf(pos.pieceOn(toSq(m))));
    const Piece mover = pos.pieceOn(fromSq(m));
    const int attackerValue = isPromotion(m) ? pieceValue(promotionType(m))
                                             : pieceValue(typeOf(mover));
    if (opts_.useSeeOrdering) {
      countQ(true, &SearchStats::qSeeCalls);
      const int see = seeCapture(pos, m);
      scores[i] = see >= 0 ? order::kGoodCapture + see * 16
                           : order::kLosingCapture + victimValue * 16 - attackerValue;
    } else {
      scores[i] = victimValue * 16 - attackerValue;
    }
  }
}

// --- Quiescence search ------------------------------------------------------

int Search::qsearch(Position& pos, int alpha, int beta, int ply) {
  countQNode();
  if (ply > seldepth_) seldepth_ = ply;
  pvLength_[ply] = ply;

  if (ply > 0 ? pos.isDrawByRule() : isPositionallyDrawn(pos)) return 0;

  // Mate distance pruning in quiescence: a mate already found closer to the root
  // cannot be improved on from deeper in the tree.
  if (alpha < matedInScore(ply)) alpha = matedInScore(ply);
  if (beta > mateInScore(ply + 1)) beta = mateInScore(ply + 1);
  if (alpha >= beta) return alpha;

  const bool inCheck = pos.inCheck();
  if (inCheck) countQ(true, &SearchStats::qInCheckNodes);
  int best;

  if (inCheck) {
    // In check there is no stand-pat: the position has to be resolved, because
    // evaluating it as if nothing were happening would treat a losing position
    // as fine. Every legal reply is searched instead.
    best = -kValueInfinity;
  } else {
    countQ(true, &SearchStats::qEvalCalls);
    best = evaluate(pos);
    if (best >= beta) return best;  // fail-soft: beta is a lower bound we beat
    if (best > alpha) alpha = best;
  }

  countQ(true, &SearchStats::qMovegenCalls);
  Move moves[256];
  int count = 0;
  // When not in check, quiescence will only ever search captures and promotions,
  // so they are generated directly instead of being generated and then discarded.
  // The set of moves returned is identical, so the searched tree is unchanged and
  // node counts must stay bit-identical; that is the check that this is a pure
  // saving rather than a behaviour change.
  //
  // When in check, EVERY legal reply is needed, including quiet ones, because
  // standing pat is not available and the position has to be resolved properly.
  const bool capturesOnly = !inCheck && opts_.useQsearchCaptureGeneration;
  generateLegal(pos, moves, count, capturesOnly);
  countQAdd(true, &SearchStats::qMovesGenerated, uint64_t(count));

  if (count == 0) {
    // Only terminal while in check. With capture-only generation a count of zero
    // in an unchecked position means "no captures available", which is the
    // common case, not a stalemate. Returning zero here would score almost every
    // quiet quiescence leaf as a draw.
    //
    // An unchecked position with genuinely no legal move is already handled: its
    // stand-pat score above is returned below.
    if (inCheck) return matedInScore(ply);
    return best;
  }

  // Select the moves quiescence is allowed to look at. When in check that is
  // every evasion, quiet ones included; otherwise only captures and
  // promotions, because a quiet move is exactly what stand-pat already assumed.
  int scores[256];
  int searchCount;
  if (inCheck) {
    // Resolving a check is forced, so there is nothing to order against: every
    // legal reply is searched.
    searchCount = count;
    for (int i = 0; i < count; ++i) scores[i] = 0;
  } else {
    scoreQuiescenceMoves(pos, moves, count, scores);
    // Compact, keeping only captures and promotions. With capture-only
    // generation this keeps everything, and the loop is a no-op that costs one
    // pass; it is kept because generateLegal may also have been called with the
    // option switched off.
    int keep = 0;
    for (int i = 0; i < count; ++i) {
      if (pos.captureOrPromotion(moves[i])) {
        moves[keep] = moves[i];
        scores[keep] = scores[i];
        ++keep;
      }
    }
    searchCount = keep;
  }
  countQAdd(true, &SearchStats::qMovesSearched, uint64_t(searchCount));
  // One selection-sort pass per move searched. Summed, this is the ordering work
  // the quiescence node performs before it recurses.
  countQAdd(true, &SearchStats::qSelectionSortPasses, uint64_t(searchCount * searchCount));

  for (int i = 0; i < searchCount; ++i) {
    // Selection sort by remaining score. Positions have at most a few dozen
    // legal moves, and selecting the maximum repeatedly avoids shuffling the
    // move and score arrays around.
    int bestIdx = i;
    for (int j = i + 1; j < searchCount; ++j)
      if (scores[j] > scores[bestIdx]) bestIdx = j;
    if (bestIdx != i) {
      std::swap(moves[i], moves[bestIdx]);
      std::swap(scores[i], scores[bestIdx]);
    }

    const Move m = moves[i];
    pos.doMove(m);
    countQ(true, &SearchStats::qMakeUnmake);
    const int score = -qsearch(pos, -beta, -alpha, ply + 1);
    pos.undoMove(m);

    if (score > best) {
      best = score;
      pvTable_[ply][ply] = m;
      for (int j = ply + 1; j < pvLength_[ply + 1]; ++j) pvTable_[ply][j] = pvTable_[ply + 1][j];
      pvLength_[ply] = pvLength_[ply + 1];
    }
    if (score > alpha) alpha = score;
    if (alpha >= beta) break;
    if ((stats_.qnodes & 1023) == 0 && timeUp()) {
      aborted_ = true;
      return best;
    }
  }
  return best;
}

// --- Main search ------------------------------------------------------------

int Search::searchRoot(Position& pos, int depth) {
  pvLength_[0] = 0;

  Move moves[256];
  int count = 0;
  generateLegal(pos, moves, count);
  if (count == 0) {
    rootBest_ = kMoveNone;
    return pos.inCheck() ? matedInScore(0) : 0;
  }

  Move ttMove = kMoveNone;
  if (opts_.useTT) {
    const TTProbeResult r = tt_.probe(pos.key(), 0);
    if (r.hit) ttMove = r.move;
  }

  int scores[256];
  scoreMoves(pos, moves, count, scores, ttMove, /*ply=*/0);

  int best = -kValueInfinity;
  rootBest_ = kMoveNone;
  int rootAlpha = -kValueInfinity;

  for (int i = 0; i < count; ++i) {
    int bestIdx = i;
    for (int j = i + 1; j < count; ++j)
      if (scores[j] > scores[bestIdx]) bestIdx = j;
    if (bestIdx != i) {
      std::swap(moves[i], moves[bestIdx]);
      std::swap(scores[i], scores[bestIdx]);
    }

    const Move m = moves[i];
    pos.doMove(m);
    int score;
    if (opts_.usePVS && i > 0) {
      // Null window: prove the move does NOT beat alpha.
      score = -negamax(pos, depth - 1, -rootAlpha - 1, -rootAlpha, 1);
      if (score > rootAlpha && !aborted_) {
        // It might be the PV move, so re-search with a full window.
        score = -negamax(pos, depth - 1, -kValueInfinity, -rootAlpha, 1);
      }
    } else {
      score = -negamax(pos, depth - 1, -kValueInfinity, kValueInfinity, 1);
    }
    pos.undoMove(m);

    if (score > best) {
      best = score;
      rootBest_ = m;
      // Rebuild the PV from the child's, which the child has just written.
      pvTable_[0][0] = m;
      for (int j = 1; j < pvLength_[1]; ++j) pvTable_[0][j] = pvTable_[1][j];
      pvLength_[0] = pvLength_[1] > 0 ? pvLength_[1] : 1;
    }
    if (score > rootAlpha) rootAlpha = score;

    if (aborted_ || timeUp()) break;
  }
  return best;
}

int Search::negamax(Position& pos, int depth, int alpha, int beta, int ply) {
  ++stats_.nodes;
  if (ply > seldepth_) seldepth_ = ply;
  pvLength_[ply] = ply;

  // Mate distance pruning: once a shorter mate is known, anything found deeper
  // cannot be an improvement. Clamping the window is enough, because a clamped
  // window fails low on its own.
  if (ply > 0) {
    if (alpha < matedInScore(ply)) alpha = matedInScore(ply);
    if (beta > mateInScore(ply + 1)) beta = mateInScore(ply + 1);
    if (alpha >= beta) return alpha;
  }

  Move ttMove = kMoveNone;
  if (opts_.useTT && ply > 0) {
    countTtProbe();
    const TTProbeResult r = tt_.probe(pos.key(), ply);
    if (r.hit) {
      countTtHit();
      ttMove = r.move;
      // A shallower entry can still be worth having as a move to try first, but
      // its score says nothing about this node at this depth.
      if (r.depth >= depth) {
        if (r.bound == kBoundExact) {
          countTtCutoff();
          pvTable_[ply][ply] = r.move;
          pvLength_[ply] = ply + 1;
          return r.score;
        }
        if (r.bound == kBoundLower && r.score >= beta) {
          countTtCutoff();
          return r.score;
        }
        if (r.bound == kBoundUpper && r.score <= alpha) {
          countTtCutoff();
          return r.score;
        }
      }
    }
  }

  if (ply > 0 ? pos.isDrawByRule() : isPositionallyDrawn(pos)) return 0;

  const bool inCheck = pos.inCheck();

  // A check extension keeps forcing lines that end in mate from being truncated
  // by the horizon. It is the one extension in this increment; it is counted so
  // its contribution can be measured rather than assumed.
  if (inCheck && opts_.useCheckExtension) {
    ++depth;
    countCheckExtension();
  }

  if (depth <= 0) {
    if (!opts_.useQsearch) return evaluate(pos);
    return qsearch(pos, alpha, beta, ply);
  }

  Move moves[256];
  int count = 0;
  generateLegal(pos, moves, count);

  if (count == 0) return inCheck ? matedInScore(ply) : 0;

  int scores[256];
  scoreMoves(pos, moves, count, scores, ttMove, ply);
  for (int i = 0; i < count; ++i) {
    int bestIdx = i;
    for (int j = i + 1; j < count; ++j)
      if (scores[j] > scores[bestIdx]) bestIdx = j;
    if (bestIdx != i) {
      std::swap(moves[i], moves[bestIdx]);
      std::swap(scores[i], scores[bestIdx]);
    }
  }

  const int originalAlpha = alpha;
  int best = -kValueInfinity;
  Move bestMove = kMoveNone;

  for (int i = 0; i < count; ++i) {
    const Move m = moves[i];
    pos.doMove(m);
    int score;
    if (opts_.usePVS && i > 0) {
      // Null window: prove the move does NOT beat alpha.
      score = -negamax(pos, depth - 1, -alpha - 1, -alpha, ply + 1);
      if (score > alpha && score < beta && !aborted_) {
        // It might be the PV move, so re-search with a full window.
        score = -negamax(pos, depth - 1, -beta, -alpha, ply + 1);
      }
    } else {
      score = -negamax(pos, depth - 1, -beta, -alpha, ply + 1);
    }
    pos.undoMove(m);

    if (aborted_ && i == 0) return 0;

    if (score > best) {
      best = score;
      bestMove = m;
      pvTable_[ply][ply] = m;
      for (int j = ply + 1; j < pvLength_[ply + 1]; ++j) pvTable_[ply][j] = pvTable_[ply + 1][j];
      pvLength_[ply] = pvLength_[ply + 1];
    }
    if (score > alpha) alpha = score;
    if (alpha >= beta) {
      countBetaCutoff(i == 0);
      if (!pos.captureOrPromotion(m)) {
        noteKiller(m, ply);
        updateHistory(pos, m, depth);
      }
      break;
    }
    if ((stats_.nodes & 1023) == 0 && timeUp()) {
      aborted_ = true;
      return 0;
    }
  }

  if (opts_.useTT && ply > 0 && bestMove != kMoveNone && !aborted_) {
    TTEntry e;
    e.key = pos.key();
    e.depth = int8_t(depth);
    e.score = int16_t(scoreToTT(best, ply));
    // The bound must be classified against the ORIGINAL alpha. Using the alpha
    // the loop ended on would label a plain cutoff-exceeded result as EXACT,
    // which is how a transposition table starts reporting false certainties.
    e.bound = uint8_t(best <= originalAlpha  ? kBoundUpper
                      : best >= beta         ? kBoundLower
                                             : kBoundExact);
    e.move = uint16_t(bestMove);
    tt_.store(e, tt_.generation());
  }
  return best;
}

void Search::extractPv(std::vector<Move>& pv) const {
  pv.clear();
  const int len = std::min(pvLength_[0], kMaxSearchPly);
  for (int i = 0; i < len; ++i) {
    if (pvTable_[0][i] == kMoveNone) break;
    pv.push_back(pvTable_[0][i]);
  }
}

SearchResult Search::searchDepth(int depth) {
  // The caller is starting a fresh search, so a stop left over from a previous
  // one is cleared here rather than inside search(), where it could race with a
  // stop already issued for this search.
  g_searchStopRequested = false;
  SearchLimits limits;
  limits.depth = depth;
  return search(limits);
}

SearchResult Search::search(const SearchLimits& limits) {
  resetSearchState();
  // Both heuristics are cleared at the start of every search. They still
  // accumulate across the iterations of one iterative-deepening run, which is
  // where most of their value is, but carrying them across separate searches
  // would make the node count depend on what was searched before and make
  // benchmarks irreproducible.
  std::memset(killers_, 0, sizeof(killers_));
  std::memset(history_, 0, sizeof(history_));
  startTimeMs_ = nowMs();
  // A clock budget takes precedence over nothing, but an explicit movetime is an
  // exact request from a match runner and is honoured as given.
  limitMs_ = limits.movetimeMs != 0 ? limits.movetimeMs
                                    : (limits.timeLimitMs != 0
                                           ? limits.timeLimitMs
                                           : computeTimeBudget(limits, root_.sideToMove()));
  limitNodes_ = limits.nodes;
  // g_searchStopRequested is deliberately NOT cleared here. With an infinite
  // search the reader thread may already have processed a "stop" by the time this
  // function runs, and clearing the flag here would swallow it and leave the
  // search running forever. Callers clear it before starting a search instead.

  SearchResult result;
  const int maxDepth =
      limits.depth > 0 ? std::min(limits.depth, kMaxSearchPly - 2) : kMaxSearchPly - 2;

  for (int depth = 1; depth <= maxDepth; ++depth) {
    aborted_ = false;
    const int score = searchRoot(root_, depth);
    const bool completed = !aborted_;

    if (completed) {
      // Remember the answer only from an iteration that finished to the end.
      // An iteration cut short by a limit has only proved part of the tree, so
      // its move may well be wrong; the previous depth did finish, so keeping it
      // is the safe answer.
      result.best = rootBest_;
      result.score = score;
      result.depth = depth;
      result.seldepth = seldepth_;
      extractPv(result.pv);
    } else if (result.best == kMoveNone) {
      // The very first iteration was cut short, so there is no earlier answer to
      // fall back on. This needs a limit small enough that not even one move
      // could be searched.
      result.best = rootBest_;
      result.score = score;
      result.depth = depth;
      extractPv(result.pv);
    }
    result.stats = stats_;
    result.completed = completed;
    result.elapsedMs = nowMs() - startTimeMs_;

    if (!completed) break;

    // A mate distance that has already been demonstrated cannot be shortened by
    // searching deeper, so there is nothing left to learn.
    if (isMateScore(score) && mateDistanceInPlies(score) <= depth) break;

    if (limits.nodes != 0 && stats_.nodes >= limits.nodes) break;
    if (limits.movetimeMs != 0 && nowMs() - startTimeMs_ >= limits.movetimeMs) break;
  }

  result.stats = stats_;
  result.elapsedMs = nowMs() - startTimeMs_;
  // The transposition table's own counters are reported rather than the search's
  // private copies, so that a benchmark and a direct probe of the table can never
  // disagree about the same run.
  if (opts_.useTT) {
    result.stats.ttProbes = tt_.probes();
    result.stats.ttHits = tt_.hits();
  }
  result.seldepth = seldepth_;
  result.depth = std::min(result.depth, maxDepth);
  return result;
}

}  // namespace pocket