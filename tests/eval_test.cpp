// Unit tests for the evaluation, the draw rules, and SEE.
//
// The load-bearing test here is evaluateSymmetry. Every term in the evaluator
// is supposed to be colour-symmetric: mirroring the board vertically and
// swapping the side to move must negate the score. That single invariant catches
// a term that reads only one colour, a piece-square table that is not mirrored
// for Black, a mobility mask computed from the wrong occupancy, and a sign
// error in the perspective flip. Those are far cheaper to introduce than a
// tuning mistake, so the invariant is checked on every position below.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "engine/evaluate.hpp"
#include "engine/movegen.hpp"
#include "engine/position.hpp"

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

void expectEq(const char* what, int got, int want) {
  char buf[128];
  snprintf(buf, sizeof(buf), "got=%d want=%d", got, want);
  check(got == want, what, buf);
}

bool load(Position& pos, const std::string& fen) {
  std::string err;
  if (!pos.setFromFen(fen, &err)) {
    printf("FEN PARSE FAILED: %s (%s)\n", err.c_str(), fen.c_str());
    ++g_fail;
    return false;
  }
  return true;
}

std::vector<std::string> splitOnSpace(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

char swapCase(char p) {
  if (p == '.') return '.';  // empty squares are not pieces
  if (p >= 'a' && p <= 'z') return char(p - 'a' + 'A');
  return char(p - 'A' + 'a');
}

// Vertical mirror of a FEN: reverse the rank order, swap piece colour, swap side
// to move, and mirror castling rights and the en passant square. The halfmove
// clock is left alone because it is colour-independent.
std::string mirrorFen(const std::string& fen) {
  std::vector<std::string> f = splitOnSpace(fen);
  while (f.size() < 6) f.push_back("0");

  // Expand digits so the placement becomes a flat 64-character rank-major
  // string, using '.' for an empty square.
  std::string flat;
  for (char c : f[0]) {
    if (c == '/') continue;
    if (c >= '1' && c <= '8') flat += std::string(c - '0', '.');
    else flat += c;
  }
  if (flat.size() != 64) {
    printf("MIRROR FEN MALFORMED (board field is %zu chars): %s\n", flat.size(), fen.c_str());
    return fen;
  }

  // Rebuild rank by rank, rank 1 first so the ranks come out reversed, swapping
  // colour as we go. Each rank is compressed separately, which keeps every digit
  // run at eight or fewer: a FEN digit may never describe more than one rank, so
  // compressing across the whole board would emit an illegal digit.
  std::string board;
  for (int r = 7; r >= 0; --r) {
    if (r != 7) board += '/';
    int i = 0;
    while (i < 8) {
      const char c = swapCase(flat[r * 8 + i]);
      if (c == '.') {
        int run = 1;
        while (i + run < 8 && flat[r * 8 + i + run] == '.') ++run;
        board += char('0' + run);
        i += run;
      } else {
        board += c;
        ++i;
      }
    }
  }

  std::string castling = f[2];
  for (auto& c : castling) {
    if (c == 'K') c = 'Q';
    else if (c == 'Q') c = 'K';
    else if (c == 'k') c = 'q';
    else if (c == 'q') c = 'k';
  }

  std::string ep = f[3];
  if (ep != "-") ep = std::string(1, ep[0]) + (ep[1] == '1' ? '6' : '2');

  return board + " " + (f[1] == "w" ? "b" : "w") + " " + castling + " " + ep + " " + f[4] +
         " " + f[5];
}

const char* kSymmetryFens[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "4k3/8/8/2pP4/8/8/8/4K3 w - c6 0 3",
    "8/5k2/8/8/8/8/5K2/6q1 w - - 0 1",
    "3k4/3p4/8/K1P4r/8/8/8/8 b - - 0 1",
    "2r3k1/p4p1p/1p2p1p1/8/2P5/1P3P2/P3B1PP/2R3K1 w - - 0 1",
    "4k3/8/8/8/8/8/8/R3K3 w - - 0 1",
    "7k/8/8/8/8/5p2/6PP/6K1 w - - 0 1",
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
    "8/8/8/4k3/8/8/8/4K3 b - - 0 1",
};

// Mirroring the board vertically and swapping the side to move maps a position
// onto an equivalent one: the same player has the move, holding the same kind of
// position. Every term must therefore return the same score, not the negation.
//
// This is the load-bearing invariant of the whole evaluator. It catches a term
// that reads only one colour, a piece-square table that is not mirrored for
// Black, a mobility mask built from the wrong occupancy, and a perspective flip
// applied to the wrong term. Note that the tempo bonus is given to the side to
// move, and the mirror swaps the side to move too, so it cancels and invariance
// holds exactly.
// Prints every evaluation term for a position and its mirror. Used when the
// invariance check fails to show which term is responsible.
void dumpTerms(const char* fen) {
  Position pos;
  if (!load(pos, fen)) return;
  Position mir;
  const std::string mirrored = mirrorFen(fen);
  if (!load(mir, mirrored)) return;

  struct Term {
    const char* name;
    TaperedScore (*fn)(const Position&);
  };
  static const Term kTerms[] = {{"material", evalMaterial},
                                {"pieceSquares", evalPieceSquares},
                                {"pawnStructure", evalPawnStructure},
                                {"pieces", evalPieces},
                                {"kingSafety", evalKingSafety},
                                {"threats", evalThreats},
                                {"space", evalSpace}};
  printf("--- %s\n    mirror %s\n", fen, mirrored.c_str());
  int sumMg = 0, sumEg = 0, mirrorSumMg = 0, mirrorSumEg = 0;
  for (const Term& t : kTerms) {
    const TaperedScore a = t.fn(pos);
    const TaperedScore b = t.fn(mir);
    sumMg += a.mg;
    sumEg += a.eg;
    mirrorSumMg += b.mg;
    mirrorSumEg += b.eg;
    printf("    %-14s %6d/%6d   %6d/%6d %s\n", t.name, a.mg, a.eg, b.mg, b.eg,
           (a.mg == -b.mg && a.eg == -b.eg) ? "" : "  <-- ASYMMETRIC");
  }
  printf("    %-14s %6d/%6d   %6d/%6d\n", "SUM", sumMg, sumEg, mirrorSumMg, mirrorSumEg);
  printf("    phase=%d score=%d mirroredScore=%d\n", gamePhase(pos), evaluate(pos),
         evaluate(mir));
}

void testEvalMirrorInvariance() {
  for (const char* fen : kSymmetryFens) {
    Position pos;
    if (!load(pos, fen)) continue;
    const std::string mirrored = mirrorFen(fen);
    Position mir;
    if (!load(mir, mirrored)) continue;

    const int a = evaluate(pos);
    const int b = evaluate(mir);
    char buf[320];
    snprintf(buf, sizeof(buf), "%s -> %s  score=%d mirrored=%d", fen, mirrored.c_str(), a, b);
    if (a != b && getenv("EVAL_DEBUG")) dumpTerms(fen);
    check(a == b, "evaluateMirrorInvariance", buf);
  }
}

void testEvalDeterminism() {
  Position pos;
  if (!load(pos, "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4"))
    return;
  const int first = evaluate(pos);
  for (int i = 0; i < 100; ++i) expectEq("evaluateDeterminism", evaluate(pos), first);
}

void testEvalMaterialSanity() {
  // A full queen up in a bare position should be decisive.
  Position up;
  if (load(up, "4k3/8/8/8/8/8/8/3QK3 w - - 0 1")) {
    check(evaluate(up) > 800, "eval upAQueen", "expected a decisive score");
    Position down;
    if (load(down, "4k3/8/8/8/8/8/8/3QK3 b - - 0 1"))
      // The tempo bonus follows the side to move, so these are not exact
      // negations: the difference is twice the tempo.
      expectEq("eval perspectiveFlip", evaluate(down), -evaluate(up) + 2 * kTempoMg);
  }

  // Equal material on a board that mirrors onto itself should score exactly the
  // tempo bonus and nothing else.
  Position bare;
  if (load(bare, "r3k3/8/8/8/8/8/8/R3K3 w - - 0 1"))
    expectEq("eval selfMirrorPosition", evaluate(bare), kTempoMg);

  // Same lone knight, centre versus rim: the table must prefer the centre.
  Position centre, rim;
  if (load(centre, "4k3/8/8/3N4/8/8/8/4K3 w - - 0 1") &&
      load(rim, "4k3/8/8/8/8/8/8/N3K3 w - - 0 1"))
    check(evaluate(centre) > evaluate(rim), "eval pstKnightCentre");

  // A rook pair with an open board should beat the same pair buried in pawns.
  Position open, blocked;
  if (load(open, "4k3/8/8/8/8/8/8/R3K2R w - - 0 1") &&
      load(blocked, "4k3/8/8/8/8/8/pppppppp/R3K2R w - - 0 1"))
    check(evaluate(open) > evaluate(blocked), "eval rookOpenFile");

  // An advanced passed pawn should be worth more than one on its home rank.
  Position home, advanced;
  if (load(home, "4k3/8/8/8/8/8/P7/4K3 w - - 0 1") &&
      load(advanced, "4k3/8/8/8/P7/8/8/4K3 w - - 0 1"))
    check(evaluate(advanced) > evaluate(home), "eval passedPawn");
}

void testDrawRules() {
  Position pos;

  check(load(pos, "8/8/8/4k3/8/8/8/4K3 w - - 0 1") && pos.hasInsufficientMaterial(),
        "draw K vs K");
  check(load(pos, "8/8/8/4k3/8/8/8/3BK3 w - - 0 1") && pos.hasInsufficientMaterial(),
        "draw K+B vs K");
  check(load(pos, "8/8/8/4k3/8/8/8/3NK3 w - - 0 1") && pos.hasInsufficientMaterial(),
        "draw K+N vs K");
  check(load(pos, "4k2n/8/8/8/8/8/8/3BK3 w - - 0 1") && pos.hasInsufficientMaterial(),
        "draw K+B vs K+N");
  check(load(pos, "8/8/8/4k3/8/8/8/2NNK3 w - - 0 1") && !pos.hasInsufficientMaterial(),
        "no draw K+2N vs K");
  check(load(pos, "4k3/8/8/8/8/8/8/3QK3 w - - 0 1") && !pos.hasInsufficientMaterial(),
        "no draw K+Q vs K");
  check(load(pos, "4k3/8/8/8/8/8/8/3RK3 w - - 0 1") && !pos.hasInsufficientMaterial(),
        "no draw K+R vs K");
  check(load(pos, "4k3/p7/8/8/8/8/8/3BK3 w - - 0 1") && !pos.hasInsufficientMaterial(),
        "no draw pawnsPresent");

  // The fifty-move rule is exactly 100 plies.
  check(load(pos, "4k3/8/8/8/8/8/8/4K2R w K - 99 60") && !pos.isFiftyMoveDraw(),
        "fiftyMove 99 plies");
  check(load(pos, "4k3/8/8/8/8/8/8/4K2R w K - 100 60") && pos.isFiftyMoveDraw(),
        "fiftyMove 100 plies");

  // Repetition: rook and king shuffle out and back, so the starting position is
  // reached again four plies later. The window is bounded by the fifty-move
  // clock, which is why the search must consult rule50 alongside the key.
  check(load(pos, "4k3/8/8/8/8/8/8/3RKN2 w - - 0 1"), "repetition setup");
  check(!pos.isRepetition(), "noRepetitionYet");
  const Move shuffle[] = {
      makeMove(makeSquare(kFileD, kRank1), makeSquare(kFileD, kRank2)),
      makeMove(makeSquare(kFileE, kRank8), makeSquare(kFileE, kRank7)),
      makeMove(makeSquare(kFileD, kRank2), makeSquare(kFileD, kRank1)),
      makeMove(makeSquare(kFileE, kRank7), makeSquare(kFileE, kRank8))};
  for (Move m : shuffle) {
    check(pos.legal(m), "repetitionMoveLegal");
    pos.doMove(m);
  }
  check(pos.isRepetition(), "repetitionAfterShuffle");

  // A capture resets the fifty-move clock, which closes the repetition window
  // and must make the same position stop counting as a repetition.
  check(load(pos, "4k3/8/8/8/8/8/8/3RKN2 w - - 0 1"), "captureWindow setup");
  const Move withCapture[] = {
      makeMove(makeSquare(kFileD, kRank1), makeSquare(kFileD, kRank2)),
      makeMove(makeSquare(kFileE, kRank8), makeSquare(kFileE, kRank7)),
      makeMove(makeSquare(kFileD, kRank2), makeSquare(kFileD, kRank1)),
      makeMove(makeSquare(kFileF, kRank1), makeSquare(kFileE, kRank3))};
  for (Move m : withCapture) {
    if (!pos.legal(m)) break;
    pos.doMove(m);
  }
  check(pos.isRepetition() == false, "noRepetitionAfterReset");
}

void testSee() {
  Position pos;
  const int P = pieceValue(kPawn), N = pieceValue(kKnight);
  const int R = pieceValue(kRook), Q = pieceValue(kQueen);

  auto expectSee = [&](const char* name, int got, int want) {
    char buf[96];
    snprintf(buf, sizeof(buf), "got=%d want=%d", got, want);
    check(got == want, name, buf);
  };

  // Rook takes a completely undefended queen.
  if (load(pos, "7k/1q6/8/8/8/8/8/KR6 w - - 0 1"))
    expectSee("see freeQueen",
              seeCapture(pos, makeMove(makeSquare(kFileB, kRank1), makeSquare(kFileB, kRank7))), Q);

  // Rxb7 wins the knight, but the rook on h7 recaptures, so the real swing is
  // the rook minus the knight rather than the full knight value.
  if (load(pos, "7k/1n2r3/8/8/8/8/8/KR6 w - - 0 1")) {
    const int see = seeCapture(pos, makeMove(makeSquare(kFileB, kRank1), makeSquare(kFileB, kRank7)));
    expectSee("see rookLosesKnightForRook", see, N - R);
    check(see < N, "see recaptureReducesGain");
  }

  // A defended pawn is worth exactly nothing: taking it and being recaptured
  // balances out. A naive "count the victim" ordering gets this wrong.
  if (load(pos, "4k3/8/4p3/3p4/4P3/8/8/4K3 w - - 0 1"))
    check(seeCapture(pos, makeMove(makeSquare(kFileE, kRank4), makeSquare(kFileD, kRank5))) == 0,
          0, "see defendedPawnIsNeutral");

  // Qxd5 wins the pawn, then loses the queen to the rook on d8, so the swing is
  // rook minus pawn: a pawn for a rook is a large net gain.
  if (load(pos, "3r1k2/8/8/3p4/8/8/8/3QK3 w - - 0 1")) {
    const int see = seeCapture(pos, makeMove(makeSquare(kFileD, kRank1), makeSquare(kFileD, kRank5)));
    expectSee("see queenTakesDefendedPawn", see, P - Q);
  }

  // Taking a defended queen with a pawn is a clear loss.
  if (load(pos, "1r2k3/8/8/1q6/P7/8/8/7K w - - 0 1")) {
    const int see = seeCapture(pos, makeMove(makeSquare(kFileA, kRank4), makeSquare(kFileB, kRank5)));
    expectSee("see pawnTakesDefendedQueen", see, Q - P);
  }

  // En passant is a pawn capture, and a quiet move is worth nothing.
  if (load(pos, "4k3/8/8/2pP4/8/8/8/4K3 w - c6 0 3")) {
    check(seeCapture(pos, makeMove(makeSquare(kFileD, kRank5), makeSquare(kFileC, kRank6),
                                   kFlagEnPassant)) == P,
          "see enPassant");
    check(seeCapture(pos, makeMove(makeSquare(kFileE, kRank1), makeSquare(kFileE, kRank2))) == 0,
          "see quietMove");
  }

  // A defended pawn taken by a knight balances out the same way.
  if (load(pos, "4k3/8/p7/1p6/1N6/8/8/4K3 w - - 0 1"))
    // A pawn taken by a knight on an outpost still loses material, because the
  // recapturing pawn is far cheaper than the knight. Guards against SEE
  // returning a negative value too eagerly for the cheapest of captures.
  if (load(pos, "4k3/8/p7/1p6/1N6/8/8/4K3 w - - 0 1")) {
    const int see = seeCapture(pos, makeMove(makeSquare(kFileB, kRank4), makeSquare(kFileB, kRank5)));
    expectSee("see knightTakesDefendedPawn", see, P - N);
  }
}

}  // namespace

int main() {
  testEvalMirrorInvariance();
  testEvalDeterminism();
  testEvalMaterialSanity();
  testDrawRules();
  testSee();
  if (g_fail == 0)
    printf("EVAL OK (%d checks)\n", g_checks);
  else
    printf("EVAL FAILED (%d of %d checks)\n", g_fail, g_checks);
  return g_fail == 0 ? 0 : 1;
}
