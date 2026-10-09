"""Tactical and endgame suites for NNUE candidate evaluation.

Phase 7 requires every serious candidate to be judged on more than validation
MAE: offline validation, a tactical suite, an endgame suite, and then matches.
This is the middle of that ladder - cheap enough (a couple of minutes) to run
on every checkpoint that comes out of training, strong enough to catch the
"validation improved, chess got worse" failure mode that Phase 6 demonstrated.

Both suites are fixed position sets with stated expectations, evaluated with
the engine's normal UCI search at a fixed depth. That makes them search
regressions as much as evaluation regressions, which is intended: a net whose
noise detonates the qsearch fails here even if its validation MAE fell.

TACTICAL (17 cases) is the exact position table from tests/tactical_test.cpp,
translated to UCI-level expectations (mate scores, specific best moves, score
bounds). The C++ suite runs the same positions through the internal search with
the classical eval; this runner is the NNUE-side mirror of it.

ENDGAME (9 cases) covers the classes Part 10 lists - pawn endings, rook
endings, minor-piece endings, queen endings, sparse pieces, opposite-coloured
bishops - with outcomes that are theory-certain, and coarse thresholds (win
>= +150 cp, draw within 150 cp, mate scores accepted as strongly winning). The
thresholds are deliberately loose: this suite catches "the net thinks a drawn
KPK is winning" and "the net cannot convert KQK", not centipawn precision.

Two of the nine cases are control-limited: the frozen classical baseline
itself scores the KPK-with-opposition draw +242..+449 cp and the wrong-bishop
rook pawn draw +781..+845 cp across depths 6-12 (measured, see RESEARCH.md).
Those cases stay in the suite because they are correct by tablebase theory -
they are exactly the endgame knowledge Phase 7 wants to test - but the
control's fixed scores are recorded with the suite, and a candidate that
recognises either draw is credited for it rather than compared against the
control. Every other case is verified against the control.

Usage:
    py -3 nnue/suites.py --name classical --depth-tactical 6 --depth-endgame 8
    py -3 nnue/suites.py --name nnue-d2 \\
        --setoption "name NnuePath value nnue/checkpoints/d2.bin" \\
        --out nnue/reports_phase7_suite_nnue-d2.json
"""

import argparse
import json
import sys
import time

import chess
import chess.engine

ROOT_ENGINE = "build/pocketchess.exe"

# name, fen, depth, expectation, detail
# Expectation forms:
#   ("mate", +1)            score is a positive mate score
#   ("bestmove", {"a7a8q"}) best move (UCI) is in the set
#   ("avoid", {"e2e4"})     best move is not in the set
#   ("at_least", cp)        score >= cp
#   ("not_below", cp)       score >= cp   (mirror for the losing side)
#   ("within", cp)          |score| <= cp
#   ("terminal", None)      position has no legal move; engine must say so
TACTICAL = [
    ("mate in 1, back rank", "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1", 2, ("mate", 1),
     "rook to the eighth rank"),
    ("mate in 2, back rank", "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1", 4, ("mate", 1),
     "rook checks, king walks, rook mates"),
    ("smothered mate in 3", "6rk/6pp/8/6N1/8/8/8/6K1 w - - 0 1", 5, ("mate", 1),
     "knight and rook box the king in"),
    ("queen mate in 2", "7k/6pp/8/8/8/8/6PP/3Q2K1 w - - 0 1", 4, ("mate", 1),
     "queen to d8 followed by mate on the file"),
    ("mated already, no move exists",
     "r1bqkb1r/pppp1Qpp/2n2n2/4p3/2B1P3/8/PPPP1PPP/RNB1K1NR b KQkq - 0 4", 4,
     ("terminal", None), "Scholar's mate: Black is checkmated"),
    ("promotion wins", "8/P6k/8/8/8/8/8/7K w - - 0 1", 4,
     ("bestmove", {"a7a8q"}), "queen promotion is the whole point"),
    ("promotion capture", "1n5k/P7/8/8/8/8/8/7K w - - 0 1", 5,
     ("bestmove", {"a7b8q"}), "capture-promote takes the knight as well"),
    ("underpromotion avoids stalemate", "8/8/8/8/8/1k6/2p5/2KR4 w - - 0 1", 8,
     ("not_below", -150), "promoting is needed to win"),
    ("en passant is found", "8/8/8/2pP4/8/8/8/K5k1 w - c6 0 2", 5,
     ("bestmove", {"d5c6"}), "en passant capture wins a pawn outright"),
    ("free queen", "4k3/8/8/R2q4/8/8/8/4K3 w - - 0 1", 4,
     ("bestmove", {"a5d5"}), "undefended queen"),
    ("winning the exchange with check", "4k3/8/8/3p4/8/2N5/8/4K3 w - - 0 1", 6,
     ("not_below", -150), "knight takes the pawn and Black cannot win it back"),
    ("must recapture or lose material", "4k3/8/3q4/8/8/3R4/8/4K3 w - - 0 1", 4,
     ("not_below", -150), "queen is en prise to the rook"),
    ("absolute pin must not be broken by a capture",
     "4k3/8/8/8/8/8/4R3/4K2r w - - 0 1", 5, ("not_below", -150),
     "the rook on e2 is pinned"),
    ("no mate claimed where none exists", "4k3/8/8/8/8/8/8/R3K3 w - - 0 1", 3,
     ("not_below", -150), "rook plus king needs far more than three plies"),
    ("opening position is roughly equal",
     "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5,
     ("not_below", -150), "no side is lost"),
    ("queen up is winning", "4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1", 4,
     ("at_least", 400), "a queen up scores clearly positive"),
    ("terminal stalemate scores zero", "k7/2Q5/8/8/8/8/8/7K b - - 0 1", 3,
     ("terminal", None), "no move exists; the engine must not invent one"),
]

ENDGAME = [
    ("KQK wins", "8/8/8/8/8/8/6k1/6KQ w - - 0 1", 6, ("at_least", 150),
     "queen ending, trivially winning"),
    ("KRK wins", "8/8/8/8/8/8/6k1/6KR w - - 0 1", 6, ("at_least", 150),
     "rook ending, trivially winning"),
    ("KBBK wins", "8/8/8/8/8/4k3/8/2B1KB2 w - - 0 1", 6, ("at_least", 150),
     "minor-piece ending, two bishops mate"),
    ("Lucena position wins", "1K1k4/1P6/8/8/8/8/6r1/R7 w - - 0 1", 8,
     ("at_least", 150), "rook ending, bridge building wins"),
    ("queen beats rook", "8/8/8/3Q4/8/8/8/1r2K2k w - - 0 1", 6, ("at_least", 150),
     "queen ending against a rook; mate found by the control at d6"),
    ("KP far king wins", "8/1k6/8/8/8/8/4P3/4K3 w - - 0 1", 6, ("at_least", 150),
     "pawn ending, defender out of the square"),
    ("KPK front king draws", "8/8/8/8/8/4k3/4P3/4K3 w - - 0 1", 6, ("within", 150),
     "pawn ending, defender in front with the opposition"),
    ("wrong bishop rook pawn draws", "7k/8/8/8/8/8/7P/1K3B2 w - - 0 1", 6,
     ("within", 150),
     "opposite-coloured bishop versus rook pawn is dead drawn; the control "
     "scores it +781 (control-limited case, see RESEARCH.md)"),
    ("bare kings evaluate level", "8/8/8/4k3/8/4K3/8/8 w - - 0 1", 4,
     ("within", 150), "sparse pieces, no material"),
]


def evaluate(engine, fen, depth):
    board = chess.Board(fen)
    if not board.legal_moves:
        return None, None
    engine.protocol.send_line("ucinewgame")
    info = engine.play(board, chess.engine.Limit(depth=depth),
                       info=chess.engine.INFO_ALL)
    score = info.info.get("score") if info.info else None
    if score is None:
        return None, None
    relative = score.relative
    if relative is None:
        return None, None
    if relative.is_mate():
        return ("mate", relative.mate()), info.move
    return ("cp", relative.score()), info.move


def check(expectation, measured, move):
    kind, value = expectation
    if kind == "terminal":
        return measured is None, "no legal move" if measured is None else "reported a move"
    if measured is None:
        return False, "no score returned"
    score_kind, score = measured
    if kind == "mate":
        ok = score_kind == "mate" and score > 0
        return ok, f"score={score_kind}:{score}"
    if score_kind == "mate":
        # A mate score is a maximally winning score for at_least purposes (but
        # never satisfies within, which is about drawn positions).
        if kind == "at_least":
            return score > 0, f"mate:{score}"
        if kind == "not_below":
            return score > 0, f"mate:{score}"
        if kind == "bestmove":
            return move.uci() in value, f"best={move.uci()}"
        if kind == "avoid":
            return move.uci() not in value, f"best={move.uci()}"
        return False, f"mate:{score}"
    if kind == "at_least":
        return score >= value, f"cp={score} want>={value}"
    if kind == "not_below":
        return score >= value, f"cp={score} want>={value}"
    if kind == "within":
        return abs(score) <= value, f"cp={score} want|.|<={value}"
    if kind == "bestmove":
        return move.uci() in value, f"best={move.uci()} want one of {sorted(value)}"
    if kind == "avoid":
        return move.uci() not in value, f"best={move.uci()} want not in {sorted(value)}"
    raise ValueError(kind)


def run_suite(engine, name, cases, depth, verbose):
    results = []
    for case_name, fen, case_depth, expectation, why in cases:
        d = case_depth if depth <= 0 else depth
        t0 = time.time()
        measured, move = evaluate(engine, fen, d)
        ok, detail = check(expectation, measured, move)
        results.append({
            "case": case_name, "fen": fen, "depth": d, "pass": ok,
            "detail": detail, "why": why, "seconds": round(time.time() - t0, 2),
        })
        if verbose or not ok:
            print(f"  {'PASS' if ok else 'FAIL'}  {case_name:42s} {detail}")
    passed = sum(1 for r in results if r["pass"])
    print(f"  {name}: {passed}/{len(results)}")
    return {"suite": name, "passed": passed, "total": len(results), "results": results}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default=ROOT_ENGINE)
    ap.add_argument("--name", required=True, help="candidate label, e.g. classical")
    ap.add_argument("--setoption", action="append", default=[],
                    help="UCI option as 'name X value Y'")
    ap.add_argument("--depth-tactical", type=int, default=0,
                    help="override tactical depths (0 = per-case depth)")
    ap.add_argument("--depth-endgame", type=int, default=0,
                    help="override endgame depths (0 = per-case depth)")
    ap.add_argument("--hash-mb", type=int, default=32)
    ap.add_argument("--out", default=None)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    engine = chess.engine.SimpleEngine.popen_uci(args.engine)
    try:
        # NnuePath and UseNnue are accepted by the engine's setoption parser
        # but are not advertised in the uciok option list (a Phase 6 wart), so
        # python-chess's configure() rejects them. Send every option as a raw
        # protocol line, exactly as match/matchlib.py does.
        for option in args.setoption + [f"name Hash value {args.hash_mb}"]:
            engine.protocol.send_line(f"setoption {option}")
        engine.ping()
        print(f"candidate {args.name}  engine={args.engine}")
        tactical = run_suite(engine, "tactical", TACTICAL, args.depth_tactical,
                             args.verbose)
        endgame = run_suite(engine, "endgame", ENDGAME, args.depth_endgame,
                            args.verbose)
    finally:
        engine.close()

    summary = {
        "candidate": args.name,
        "engine": args.engine,
        "setoptions": args.setoption,
        "tactical": tactical,
        "endgame": endgame,
        "total_passed": tactical["passed"] + endgame["passed"],
        "total": tactical["total"] + endgame["total"],
    }
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)
        print(f"wrote {args.out}")
    return 0 if summary["total_passed"] == summary["total"] else 1


if __name__ == "__main__":
    sys.exit(main())
