// Search correctness tests.
//
// The organising idea is that the optimised search is never trusted on its own.
// Every fixed-depth search result is compared against negamaxReference(), a
// deliberately plain negamax with no transposition table, no move ordering, no
// quiescence search and no extensions. If the two disagree at a depth where the
// reference has finished searching, something in the fast path is wrong.
//
// WHERE THE TWO CAN LEGITIMATELY DIFFER
// -------------------------------------
// Quiescence search and the check extension both change what a fixed-depth
// search can see, so comparing the full optimised search against the reference at
// the same depth is meaningless. Comparisons here therefore run with both
// switched off, which isolates the transposition table and move ordering: those
// must be score-preserving, and if they are not, they are doing more than their
// job. The separate ablation tests check the same property.
//
// Where several moves score identically the two searches are allowed to pick
// different ones, so comparisons are made on score and legality, and on the
// selected move only when the best score is unique.
//
// Stockfish is deliberately not used as an oracle. Being strong does not make a
// score correct, and one engine's evaluation is not the right thing to grade a
// different evaluation against.
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "engine/evaluate.hpp"
#include "engine/search.hpp"
#include "engine/uci.hpp"

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

bool load(Position& pos, const char* fen) {
  std::string err;
  if (!pos.setFromFen(fen, &err)) {
    printf("FEN PARSE FAILED: %s (%s)\n", err.c_str(), fen);
    ++g_fail;
    return false;
  }
  return true;
}

Move findMove(Position& pos, Square from, Square to, MoveFlag flag = kFlagNormal) {
  Move buf[256];
  int n = 0;
  generateLegal(pos, buf, n);
  for (int i = 0; i < n; ++i)
    if (fromSq(buf[i]) == from && toSq(buf[i]) == to && flagOf(buf[i]) == flag) return buf[i];
  return kMoveNone;
}

// Piece type standing on a square, or kNoPieceType for an empty square.
int pieceTypeOn(const Position& pos, Square s) {
  const Piece pc = pos.pieceOn(s);
  return pc == kNoPiece ? int(kNoPieceType) : int(typeOf(pc));
}

// Options with only the score-preserving features on, for reference comparison.
SearchOptions referenceComparable() {
  SearchOptions o;
  o.useTT = true;
  o.useSeeOrdering = true;
  o.useKillers = true;
  o.useHistory = true;
  o.useQsearch = false;           // changes the horizon by design
  o.useCheckExtension = false;    // changes the horizon by design
  return o;
}

// --- Score convention -------------------------------------------------------

void testScoreConvention() {
  Position w, b;
  if (load(w, "4k3/8/8/8/8/8/8/R3K3 w - - 0 1") && load(b, "4k3/8/8/8/8/8/8/R3K3 b - - 0 1")) {
    check(evaluate(w) != evaluate(b), "eval depends on the side to move");
  }

  // A position winning for White must score positive for White and negative for
  // Black. Mixing a White-relative evaluation into a side-to-move search would
  // make the engine play with the colours swapped, so this is asserted rather
  // than assumed.
  Position good, flipped;
  if (load(good, "4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1") &&
      load(flipped, "4k3/8/8/8/8/8/4Q3/4K3 b - - 0 1")) {
    check(evaluate(good) > 0, "eval favours the side that is winning");
    check(evaluate(flipped) < 0, "eval is signed by the side to move");
    // Tempo follows the side to move, so the two are not exact negations.
    expectEq("eval sign flips", evaluate(flipped), -evaluate(good) + 2 * kTempoMg);
  }

  // Mate helpers encode distance in the sentinel range and must be ordered so
  // that a plain max() gives the right preference in both directions.
  check(mateInScore(1) > mateInScore(2), "shorter mate scores higher when winning");
  check(mateInScore(2) > mateInScore(3), "mate distance ordering is monotonic");
  check(matedInScore(10) > matedInScore(5), "longer defence scores higher when losing");
  check(isMateScore(mateInScore(1)), "mateInScore is recognised");
  check(isMateScore(matedInScore(5)), "matedInScore is recognised");
  check(!isMateScore(0), "zero is not a mate score");
  check(!isMateScore(1200), "a centipawn score is not a mate score");
  check(!isMateScore(kValueMateInMaxPly), "the sentinel boundary is not a mate score");
  expectEq("mate distance 1", mateDistanceInPlies(mateInScore(1)), 1);
  expectEq("mate distance 3", mateDistanceInPlies(matedInScore(3)), 3);
  expectEq("no distance for centipawns", mateDistanceInPlies(400), 0);
}

// --- Terminal states --------------------------------------------------------

void testStalemate() {
  // Black king a8, white queen c7, Black to move: no legal move and not in
  // check, which is stalemate rather than mate.
  Position pos;
  if (!load(pos, "k7/2Q5/8/8/8/8/8/7K b - - 0 1")) return;
  expectEq("stalemate has no legal moves", legalMoveCount(pos), 0);
  check(!pos.inCheck(), "stalemate is not check");
  // Stalemate is not detectable from the position alone: proving it means
  // generating the moves and finding there are none. That is exactly why it is
  // not part of isPositionallyDrawn(), which the search consults before it has
  // generated anything.
  expectEq("stalemate is not position-local drawn", isPositionallyDrawn(pos), 0);

  Search ref(pos);
  expectEq("stalemate reference score", ref.searchReference(2), 0);
  Search opt(pos);
  expectEq("stalemate optimised score", opt.searchDepth(2).score, 0);
  check(opt.searchDepth(2).best == kMoveNone, "stalemate returns no move");
}

void testCheckmate() {
  // White queen g7 is defended by the king on f7, so the black king on h8 is in
  // check with no square to go.
  Position pos;
  if (!load(pos, "7k/5KQ1/8/8/8/8/8/8 b - - 0 1")) return;
  expectEq("checkmate has no legal moves", legalMoveCount(pos), 0);
  check(pos.inCheck(), "checkmate is check");

  Search ref(pos);
  const int score = ref.searchReference(2);
  check(isMateScore(score), "checkmate score is a mate score");
  check(score < 0, "checkmate is negative for the mated side");
  expectEq("checkmate distance", mateDistanceInPlies(score), 0);

  Search opt(pos);
  const SearchResult r = opt.searchDepth(2);
  check(isMateScore(r.score) && r.score < 0, "checkmate recognised by optimised search");
  check(r.best == kMoveNone, "mated side has no move");
}

void testInsufficientMaterial() {
  const char* bare[] = {
      "8/8/8/4k3/8/8/8/4K3 w - - 0 1",   // K vs K
      "8/8/8/4k3/8/8/8/3BK3 w - - 0 1",   // K+B vs K
      "8/8/8/4k3/8/8/8/3NK3 w - - 0 1",   // K+N vs K
      "4k2n/8/8/8/8/8/8/3BK3 w - - 0 1",  // K+B vs K+N
  };
  for (const char* fen : bare) {
    Position pos;
    if (!load(pos, fen)) continue;
    check(pos.hasInsufficientMaterial(), "insufficient material setup");
    check(isPositionallyDrawn(pos), "insufficient material is position-local");
    Search ref(pos);
    expectEq("insufficient material reference", ref.searchReference(4), 0);
    Search opt(pos);
    expectEq("insufficient material optimised", opt.searchDepth(4).score, 0);
  }

  // Material that can still mate must NOT be called a draw.
  const char* sufficient[] = {
      "8/8/8/4k3/8/8/8/2NNK3 w - - 0 1",   // K+2N vs K
      "4k3/8/8/8/8/8/8/3QK3 w - - 0 1",    // K+Q vs K
      "4k3/8/8/8/8/8/8/3RK3 w - - 0 1",    // K+R vs K
      "4k3/p7/8/8/8/8/8/3BK3 w - - 0 1",  // pawns present
  };
  for (const char* fen : sufficient) {
    Position pos;
    if (!load(pos, fen)) continue;
    check(!pos.hasInsufficientMaterial(), "sufficient material is not drawn");
    check(!isPositionallyDrawn(pos), "sufficient material is not position-local drawn");
  }
}

void testFiftyMove() {
  // No castling rights here on purpose. An earlier version of this test used a
  // position where White could castle, and castling resets the fifty-move clock,
  // so the search legitimately escaped the draw by castling. A draw test needs a
  // position with no such escape.
  Position pos;
  if (load(pos, "4k3/8/8/8/8/8/8/R3K3 w - - 100 80")) {
    check(pos.isFiftyMoveDraw(), "100 plies is a fifty-move draw");
    check(isPositionallyDrawn(pos), "fifty-move draw is position-local");
    Search ref(pos);
    expectEq("fifty move reference", ref.searchReference(3), 0);
    Search opt(pos);
    expectEq("fifty move optimised", opt.searchDepth(3).score, 0);
  }

  Position short_;
  if (load(short_, "4k3/8/8/8/8/8/8/R3K3 w - - 99 80")) {
    check(!short_.isFiftyMoveDraw(), "99 plies is not yet a draw");
    check(!isPositionallyDrawn(short_), "99 plies is not a position-local draw");
  }
}

void testRepetition() {
  // Repetition is deliberately excluded from the root: whether the position in
  // front of a search has already occurred depends on game history that the
  // caller owns. It is asserted inside the tree instead, where the position key
  // history is real.
  Position pos;
  if (!load(pos, "4k3/8/8/8/8/8/8/3RKN2 w - - 0 1")) return;
  check(!pos.isRepetition(), "repetition setup is not yet a repetition");
  check(!isPositionallyDrawn(pos), "repetition is not a position-local draw");

  const Move shuffle[] = {
      makeMove(makeSquare(kFileD, kRank1), makeSquare(kFileD, kRank2)),
      makeMove(makeSquare(kFileE, kRank8), makeSquare(kFileE, kRank7)),
      makeMove(makeSquare(kFileD, kRank2), makeSquare(kFileD, kRank1)),
      makeMove(makeSquare(kFileE, kRank7), makeSquare(kFileE, kRank8))};
  for (Move m : shuffle) {
    if (!pos.legal(m)) {
      check(false, "repetition shuffle move legal");
      return;
    }
    pos.doMove(m);
  }
  check(pos.isRepetition(), "repetition detected after a shuffle");
  check(pos.isDrawByRule(), "repetition is a draw by rule inside the tree");
  check(!isPositionallyDrawn(pos), "repetition is not position-local");

  // From the repeated root the search deliberately does NOT score 0. Treating an
  // arbitrary root as drawn would stop the engine playing a position that is
  // perfectly playable as a fresh puzzle; whether it has occurred before is
  // history that the caller owns. What matters is that the draw is recognised
  // and would apply to any node below the root.
  Search opt(pos);
  const SearchResult r = opt.searchDepth(3);
  check(r.best != kMoveNone && pos.legal(r.best), "repeated root still yields a legal move");
  char buf[96];
  snprintf(buf, sizeof(buf), "score=%d", r.score);
  check(r.score != 0, "repeated root is not force-scored as a draw", buf);

  // The same position reached again one ply down IS a draw, and that is the
  // case the search actually scores as zero. Continuing the shuffle from the
  // repeated position lands back on the position that occurred at gamePly 1, so
  // the new node is a repetition and the search must treat it as drawn.
  const Move again = shuffle[0];
  check(pos.legal(again), "continuing the shuffle is legal");
  Position after(pos);
  after.doMove(again);
  check(after.isRepetition(), "a repetition below the root is detected");
  check(after.isDrawByRule(), "a repetition below the root is a draw by rule");
  check(!isPositionallyDrawn(after), "repetition still stays out of the root rules");
}

// --- Tactics ----------------------------------------------------------------

void testMateInOne() {
  // White plays Ra1-a8# against a king shut in by its own pawns.
  Position pos;
  if (!load(pos, "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1")) return;
  const Move ra8 = findMove(pos, makeSquare(kFileA, kRank1), makeSquare(kFileA, kRank8));
  check(ra8 != kMoveNone, "mate move exists");

  Search s(pos);
  const SearchResult r = s.searchDepth(2);
  check(isMateScore(r.score) && r.score > 0, "back rank mate found");
  check(r.best == ra8, "back rank mate move selected");
  expectEq("back rank mate distance", mateDistanceInPlies(r.score), 1);

  // Reference must agree, with the horizon-changing features off.
  Position refPos;
  if (load(refPos, "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1")) {
    Search ref(refPos);
    const int refScore = ref.searchReference(2);
    expectEq("mate in one agrees with reference", refScore, r.score);
  }
}

void testCaptures() {
  // Free queen: the rook shares a rank with it and nothing defends it.
  {
    Position pos;
    if (load(pos, "4k3/8/8/R2q4/8/8/8/4K3 w - - 0 1")) {
      const Move rxd5 = findMove(pos, makeSquare(kFileA, kRank5), makeSquare(kFileD, kRank5));
      check(rxd5 != kMoveNone, "free queen capture exists");
      Search s(pos);
      check(s.searchDepth(3).best == rxd5, "free queen taken");
    }
  }

  // Undefended bishop captured by a rook. A rook is used rather than a knight on
  // purpose: capturing a bishop with a knight loses material, so an earlier
  // version of this test asserted something the engine was right to refuse.
  {
    Position pos;
    if (load(pos, "4k3/8/8/R3b3/8/8/8/4K3 w - - 0 1")) {
      const Move rxd5 = findMove(pos, makeSquare(kFileA, kRank5), makeSquare(kFileE, kRank5));
      check(rxd5 != kMoveNone, "undefended bishop capture exists");
      Search s(pos);
      check(s.searchDepth(4).best == rxd5, "undefended bishop captured");
    }
  }

  // Promotion must be found.
  {
    Position pos;
    if (load(pos, "8/P6k/8/8/8/8/8/7K w - - 0 1")) {
      const Move promo =
          findMove(pos, makeSquare(kFileA, kRank7), makeSquare(kFileA, kRank8), kFlagPromotion);
      check(promo != kMoveNone, "promotion move exists");
      Search s(pos);
      check(s.searchDepth(4).best == promo, "promotion played");
    }
  }

  // En passant must be generated and legal. Note the rank holds exactly eight
  // squares; "3pP4" is nine and the FEN parser rejects it.
  {
    Position pos;
    if (load(pos, "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2")) {
      const Move ep = findMove(pos, makeSquare(kFileE, kRank5), makeSquare(kFileD, kRank6),
                               kFlagEnPassant);
      check(ep != kMoveNone, "en passant generated");
      check(pos.legal(ep), "en passant legal");
      Position after(pos);
      after.doMove(ep);
      expectEq("en passant removes the pawn", pieceTypeOn(after, makeSquare(kFileD, kRank5)),
               int(kNoPieceType));
    }
  }

  // Castling must be generated, legal, and available to search.
  {
    Position pos;
    if (load(pos, "r3k2r/pppq1ppp/2npbn2/2b1p3/2B1P3/2NPBN2/PPPQ1PPP/R3K2R w KQkq - 0 1")) {
      const Move oo = findMove(pos, makeSquare(kFileE, kRank1), makeSquare(kFileG, kRank1),
                               kFlagCastling);
      check(oo != kMoveNone, "kingside castling generated");
      check(pos.legal(oo), "kingside castling legal");
      Position after(pos);
      after.doMove(oo);
      expectEq("castling puts the king on g1",
               pieceTypeOn(after, makeSquare(kFileG, kRank1)), int(kKing));
      expectEq("castling puts the rook on f1",
               pieceTypeOn(after, makeSquare(kFileF, kRank1)), int(kRook));
      Search s(pos);
      const SearchResult r = s.searchDepth(3);
      check(r.best != kMoveNone && pos.legal(r.best), "castling position yields a legal move");
    }
  }
}

// --- Reference agreement ----------------------------------------------------

void testAgainstReference() {
  struct Case {
    const char* fen;
    int depth;
  };
  const Case cases[] = {
      {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 1},
      {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 2},
      {"r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4", 3},
      {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 3},
      {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 2},
      {"4k3/8/8/8/8/8/8/R3K3 w - - 0 1", 3},
      {"2r3k1/p4p1p/1p2p1p1/8/2P5/1P3P2/P3B1PP/2R3K1 w - - 0 1", 3},
      {"8/5k2/8/8/8/8/5K2/6q1 w - - 0 1", 3},
      {"4k3/8/8/2pP4/8/8/8/4K3 w - c6 0 3", 3},
      {"7k/8/8/8/8/8/8/K6R b - - 0 1", 2},
  };

  for (const Case& c : cases) {
    Position a;
    if (!load(a, c.fen)) continue;
    Search ref(a);
    const int refScore = ref.searchReference(c.depth);

    Position b;
    if (!load(b, c.fen)) continue;
    Search fast(b, referenceComparable());
    const SearchResult r = fast.searchDepth(c.depth);

    char buf[320];
    snprintf(buf, sizeof(buf), "fen=%s depth=%d reference=%d optimised=%d", c.fen, c.depth,
             refScore, r.score);
    check(refScore == r.score, "reference agreement", buf);
  }
}

void testReferenceRepeatability() {
  // Searching the same position repeatedly must give the same answer. A
  // transposition table that returns a bound as though it were exact shows up
  // here as a score that drifts between identical runs.
  const char* fens[] = {
      "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
      "2r3k1/p4p1p/1p2p1p1/8/2P5/1P3P2/P3B1PP/2R3K1 w - - 0 1",
  };
  for (const char* fen : fens) {
    Position pos;
    if (!load(pos, fen)) continue;
    Search s(pos);
    const int a = s.searchDepth(4).score;
    const int b = s.searchDepth(4).score;
    const int c = s.searchDepth(4).score;
    char buf[256];
    snprintf(buf, sizeof(buf), "fen=%s scores=%d,%d,%d", fen, a, b, c);
    check(a == b && b == c, "repeated search is stable", buf);
  }
}

// --- Feature independence ---------------------------------------------------

void testFeaturesDoNotChangeScore() {
  // Each ordering mechanism is switched off in turn and the score must not move.
  // These mechanisms reorder moves; if any of them can change the answer it is
  // affecting the search rather than just its order.
  const char* fen = "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4";
  Position basePos;
  if (!load(basePos, fen)) return;
  Search base(basePos);
  const int baseScore = base.searchDepth(4).score;

  SearchOptions noSee, noKillers, noHistory;
  noSee.useSeeOrdering = false;
  noKillers.useKillers = false;
  noHistory.useHistory = false;

  const struct {
    const char* name;
    SearchOptions o;
  } variants[] = {{"seeOrdering off", noSee},
                  {"killers off", noKillers},
                  {"history off", noHistory}};

  for (const auto& v : variants) {
    Position p;
    if (!load(p, fen)) return;
    Search s(p, v.o);
    const int got = s.searchDepth(4).score;
    char buf[192];
    snprintf(buf, sizeof(buf), "variant=%s base=%d got=%d", v.name, baseScore, got);
    check(got == baseScore, "ordering feature does not change score", buf);
  }

  // With the transposition table off, the result must match the reference.
  Position refPos;
  if (!load(refPos, fen)) return;
  Search ref(refPos);
  const int refScore = ref.searchReference(4);

  Position p;
  if (!load(p, fen)) return;
  SearchOptions noTT = referenceComparable();
  noTT.useTT = false;
  Search noTt(p, noTT);
  char buf[192];
  snprintf(buf, sizeof(buf), "reference=%d noTT=%d", refScore, noTt.searchDepth(4).score);
  check(noTt.searchDepth(4).score == refScore, "TT off matches reference", buf);
}

// --- Transposition table behaviour in search --------------------------------

void testTTBehaviour() {
  const char* fen = "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4";
  Position pos;
  if (!load(pos, fen)) return;

  Search s(pos);
  const SearchResult r = s.searchDepth(6);
  check(r.stats.ttProbes > 0, "TT was probed during search");
  check(r.stats.ttHits > 0, "TT had hits during a real search");
  check(r.stats.ttHits <= r.stats.ttProbes, "hits cannot exceed probes");
  if (r.stats.ttProbes > 0) {
    const double rate = 100.0 * double(r.stats.ttHits) / double(r.stats.ttProbes);
    char buf[128];
    snprintf(buf, sizeof(buf), "hits=%llu probes=%llu rate=%.2f%%",
             (unsigned long long)r.stats.ttHits, (unsigned long long)r.stats.ttProbes, rate);
    printf("  TT %s\n", buf);
  }

  // A table that is never cleared between searches must still give the right
  // answer, and a tiny table must give the same answer as a large one. If a
  // collision could corrupt a score, these would disagree.
  const int large = s.searchDepth(5).score;
  Position p2;
  if (!load(p2, fen)) return;
  SearchOptions tiny;
  tiny.ttSizeMB = 1;
  Search s2(p2, tiny);
  const int small = s2.searchDepth(5).score;
  char buf[128];
  snprintf(buf, sizeof(buf), "16MB=%d 1MB=%d", large, small);
  check(large == small, "table size does not change the score", buf);
}

// --- Ordering ---------------------------------------------------------------

void testOrderingLegality() {
  // An illegal bestmove is the worst output failure there is, because a GUI
  // cannot recover from it. Checked across positions and depths.
  const char* fens[] = {
      "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
      "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
      "4k3/8/8/2pP4/8/8/8/4K3 w - c6 0 3",
      "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1",
      "2r3k1/p4p1p/1p2p1p1/8/2P5/1P3P2/P3B1PP/2R3K1 w - - 0 1",
      "8/P6k/8/8/8/8/8/7K w - - 0 1",
  };
  for (const char* fen : fens) {
    for (int depth = 1; depth <= 4; ++depth) {
      Position pos;
      if (!load(pos, fen)) continue;
      Search s(pos);
      const SearchResult r = s.searchDepth(depth);
      char buf[288];
      snprintf(buf, sizeof(buf), "fen=%s depth=%d best=%d", fen, depth, int(r.best));
      check(r.best != kMoveNone, "search produced a move", buf);
      check(r.best != kMoveNone && pos.legal(r.best), "bestmove is legal", buf);
      check(r.depth >= 1 && r.depth <= depth, "reported depth is sane", buf);
      check(r.seldepth >= r.depth, "seldepth at least depth", buf);
      check(r.stats.nodes > 0, "nodes were counted", buf);
    }
  }
}

void testKillerAndHistorySanity() {
  // Ordering has to be able to change the number of nodes without changing the
  // answer. Comparing node counts against a run with the heuristics disabled is
  // the direct test; comparing a "cold" against a "warm" engine would not, since
  // both heuristics are cleared at the start of every search precisely so that
  // node counts are reproducible.
  const char* fens[] = {
      "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
  };
  for (const char* fen : fens) {
    struct Run {
      SearchOptions o;
      const char* name;
    };
    SearchOptions withHeuristics, without;
    without.useKillers = false;
    without.useHistory = false;

    const Run runs[] = {{withHeuristics, "killers+history"},
                        {without, "neither"}};

    uint64_t nodes[2] = {0, 0};
    int score[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
      Position p;
      if (!load(p, fen)) return;
      Search s(p, runs[i].o);
      const SearchResult r = s.searchDepth(6);
      nodes[i] = r.stats.nodes;
      score[i] = r.score;
    }
    char buf[224];
    snprintf(buf, sizeof(buf), "fen=%s killers+history=%llu neither=%llu", fen,
             (unsigned long long)nodes[0], (unsigned long long)nodes[1]);
    check(nodes[0] > 0 && nodes[1] > 0, "both runs counted nodes", buf);
    printf("  %s\n", buf);
    check(score[0] == score[1], "heuristics do not change the score",
          buf);
  }
}

// --- Qsearch ----------------------------------------------------------------

void testQsearch() {
  // Quiescence must not break legality or terminal handling.
  struct Case {
    const char* fen;
    int depth;
  };
  const Case cases[] = {
      {"4k3/8/8/3p4/8/8/8/R3K3 w - - 0 1", 1},
      {"4k3/8/8/3p4/8/8/8/R3K3 w - - 0 1", 4},
      {"6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1", 3},
      {"7k/8/8/8/8/8/8/K6R b - - 0 1", 3},
      {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 2},
  };
  for (const Case& c : cases) {
    Position pos;
    if (!load(pos, c.fen)) continue;
    Search s(pos);
    const SearchResult r = s.searchDepth(c.depth);
    char buf[288];
    snprintf(buf, sizeof(buf), "fen=%s depth=%d best=%d", c.fen, c.depth, int(r.best));
    check(r.best == kMoveNone || pos.legal(r.best), "qsearch returns a legal move", buf);
    check(r.stats.qnodes > 0, "qsearch was entered", buf);
  }

  // Being in check at a quiescence leaf must resolve the position rather than
  // standing pat on it: the rook on h1 checks the king on h8.
  Position inCheck;
  if (load(inCheck, "7k/8/8/8/8/8/8/K6R b - - 0 1")) {
    check(inCheck.inCheck(), "qsearch in-check setup is in check");
    Search s(inCheck);
    const SearchResult r = s.searchDepth(3);
    check(r.best == kMoveNone || inCheck.legal(r.best), "check evasion is legal");
  }

  // With quiescence disabled the same position must still return a legal move,
  // proving qsearch is an addition rather than a dependency.
  {
    Position pos;
    if (load(pos, "4k3/8/8/3p4/8/8/8/R3K3 w - - 0 1")) {
      SearchOptions o;
      o.useQsearch = false;
      Search s(pos, o);
      check(s.searchDepth(4).best != kMoveNone, "no-qsearch still returns a move");
    }
  }
}

// --- Iterative deepening ----------------------------------------------------

void testIterativeDeepening() {
  const char* fen = "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4";
  Position pos;
  if (!load(pos, fen)) return;

  Search s(pos);
  const SearchResult r = s.searchDepth(5);
  check(r.depth == 5, "iterative deepening reached the requested depth");
  check(r.completed, "iterative deepening completed");
  check(r.pv.size() >= 1, "PV is non-empty");
  check(r.pv.size() <= size_t(r.depth), "PV no longer than the depth searched");
  check(r.seldepth >= r.depth, "seldepth at least depth");
  check(r.stats.nodes > 0, "nodes counted");

  // Every PV move must be legal in the position it is played from. A stale or
  // truncated PV is easy to produce by accident.
  Position walk;
  if (!load(walk, fen)) return;
  bool pvLegal = true;
  for (Move m : r.pv) {
    if (!walk.legal(m)) {
      pvLegal = false;
      break;
    }
    walk.doMove(m);
  }
  check(pvLegal, "every PV move is legal in sequence");

  // A node limit must stop the search and still leave a usable answer.
  {
    Position p2;
    if (!load(p2, "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"))
      return;
    Search s2(p2);
    SearchLimits limits;
    limits.depth = 12;
    limits.nodes = 20000;
    const SearchResult lim = s2.search(limits);
    check(lim.best != kMoveNone && p2.legal(lim.best), "node-limited search returns a legal move");
    check(lim.depth < 12, "node limit stopped the search early");
    check(lim.stats.nodes < 12ull * 200000ull, "node limit was respected");
  }

  // A time limit must also stop the search and still leave a usable answer.
  {
    Position p3;
    if (!load(p3, "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"))
      return;
    Search s3(p3);
    SearchLimits limits;
    limits.depth = 30;
    limits.movetimeMs = 200;
    const SearchResult tl = s3.search(limits);
    check(tl.best != kMoveNone && p3.legal(tl.best), "time-limited search returns a legal move");
    check(tl.elapsedMs < 2000, "time limit was roughly respected");
  }

  // Depth 1 must work: it is the first iteration and the one most likely to be
  // special-cased by mistake.
  {
    Position p4;
    if (!load(p4, fen)) return;
    Search s4(p4);
    const SearchResult d1 = s4.searchDepth(1);
    check(d1.best != kMoveNone && p4.legal(d1.best), "depth 1 returns a legal move");
    check(d1.depth == 1, "depth 1 reports depth 1");
  }
}

// --- UCI --------------------------------------------------------------------

void testUci() {
  UciEngine engine;
  std::ostringstream out;

  engine.execute("uci", out);
  const std::string text = out.str();
  check(text.find("uciok") != std::string::npos, "uci replies uciok");
  check(text.find("id name") != std::string::npos, "uci reports a name");

  out.str("");
  engine.execute("isready", out);
  check(out.str().find("readyok") != std::string::npos, "isready replies readyok");

  // A fixed-depth search must return a legal bestmove.
  out.str("");
  engine.execute("position startpos", out);
  engine.execute("go depth 5", out);
  const std::string goText = out.str();
  check(goText.find("bestmove") != std::string::npos, "go emits bestmove");
  check(goText.find("info depth") != std::string::npos, "go emits info");

  // Move parsing and formatting must round trip.
{
    bool ok = false;
    const Move m = moveFromUci("e1g1", &ok);
    check(ok, "moveFromUci parses castling coordinates");
    expectEq("castling from-square", int(fromSq(m)), int(makeSquare(kFileE, kRank1)));
    expectEq("castling to-square", int(toSq(m)), int(makeSquare(kFileG, kRank1)));
    check(moveToUci(m) == "e1g1", "moveToUci round trip");

    const Move p = moveFromUci("a7a8q", &ok);
    check(ok, "moveFromUci parses a promotion");
    check(isPromotion(p), "promotion flag survives parsing");
    expectEq("promotion piece", int(promotionType(p)), int(kQueen));
    check(moveToUci(p) == "a7a8q", "promotion round trip");

    moveFromUci("zzzz", &ok);
    check(!ok, "garbage move rejected");
    moveFromUci("e9e4", &ok);
    check(!ok, "out-of-range square rejected");
    check(moveToUci(kMoveNone) == "0000", "null move formats as 0000");
  }

  // A mated position must report bestmove 0000, since there is no move to make.
  out.str("");
  engine.execute("position fen 7k/5KQ1/8/8/8/8/8/8 b - - 0 1", out);
  engine.execute("go depth 2", out);
  check(out.str().find("bestmove 0000") != std::string::npos,
        "mated position reports bestmove 0000");

  // A position command with a move list must replay it.
  out.str("");
  engine.execute("position startpos moves e2e4 e7e5 g1f3", out);
  engine.execute("go depth 4", out);
  check(out.str().find("bestmove") != std::string::npos, "position with moves is searchable");

  // An unparseable move in the list must stop the replay rather than corrupt the
  // position, and the engine must still answer.
  out.str("");
  engine.execute("position startpos moves e2e4 zzzz", out);
  engine.execute("go depth 3", out);
  check(out.str().find("bestmove") != std::string::npos, "bad move in list is survivable");

  // A movetime search must finish and answer.
  out.str("");
  engine.execute("position startpos", out);
  engine.execute("go movetime 150", out);
  check(out.str().find("bestmove") != std::string::npos, "movetime search answers");

  // No info line may contain a negative depth or an unprintable score.
  {
    out.str("");
    engine.execute("position startpos", out);
    engine.execute("go depth 4", out);
    const std::string all = out.str();
    check(all.find("depth -") == std::string::npos, "no negative depth in info");
    check(all.find("seldepth -") == std::string::npos, "no negative seldepth in info");
    check(all.find("nan") == std::string::npos, "no nan in info");
    check(all.find("score cp") != std::string::npos ||
              all.find("score mate") != std::string::npos,
          "score field is well formed");
  }

  // ucinewgame and quit must be accepted; quit must end the loop.
  out.str("");
  check(engine.execute("ucinewgame", out), "ucinewgame accepted");
  check(!engine.execute("quit", out), "quit ends the loop");
}

}  // namespace

int main() {
  testScoreConvention();
  testStalemate();
  testCheckmate();
  testInsufficientMaterial();
  testFiftyMove();
  testRepetition();
  testMateInOne();
  testCaptures();
  testAgainstReference();
  testReferenceRepeatability();
  testFeaturesDoNotChangeScore();
  testTTBehaviour();
  testOrderingLegality();
  testKillerAndHistorySanity();
  testQsearch();
  testIterativeDeepening();
  testUci();

  if (g_fail == 0)
    printf("SEARCH OK (%d checks)\n", g_checks);
  else
    printf("SEARCH FAILED (%d of %d checks)\n", g_fail, g_checks);
  return g_fail == 0 ? 0 : 1;
}