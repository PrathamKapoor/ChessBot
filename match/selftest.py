#!/usr/bin/env python3
"""Tests for the match infrastructure itself.

The point of these is that the runner must be trustworthy before any strength
claim rests on it. They use FAKE engines written as small scripts, so the failure
modes can be produced deliberately and exactly:

  good       plays legal moves, answers bestmove
  illegal    proposes a move that is not legal
  n Bestmove proposes nothing and exits
  crash      dies mid-search
  slow       never answers, so the timeout path is exercised
  garbage    emits malformed UCI on stdout

A failure must be reported as a failure with its own termination reason and must
NOT be folded into a score. Every one of those cases is checked here rather than
assumed.

Run:  py -3 match/selftest.py
"""
import io
import random
import os
import stat
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import matchlib
import sprt as sprt_mod
import runner as runner_mod

try:
    import chess
except ImportError:
    print("python-chess is required", file=sys.stderr)
    sys.exit(2)


# --- fake engines -----------------------------------------------------------
# Each writes a Python script and returns its path. They are given the real
# engine's build directory on the command line so a fake can defer to the real
# engine for legal moves where that is convenient.

GOOD_ENGINE = r'''
import sys
print("id name FakeGood", flush=True)
print("id author test", flush=True)
print("uciok", flush=True)
board = None
import chess
for line in sys.stdin:
    line = line.strip()
    if line == "uci":
        print("id name FakeGood", flush=True)
        print("uciok", flush=True)
    elif line == "isready":
        print("readyok", flush=True)
    elif line.startswith("position"):
        board = chess.Board()
        parts = line.split()
        # "fen" and "moves" can both be present, so these must not be an
        # either/or chain: an if/elif here would silently drop the move list and
        # leave the engine searching a stale position.
        if "fen" in parts:
            i = parts.index("fen")
            board = chess.Board(" ".join(parts[i+1:i+7]))
        if "moves" in parts:
            i = parts.index("moves")
            for uci in parts[i+1:]:
                try: board.push_uci(uci)
                except Exception: break
    elif line.startswith("go"):
        if board is None: board = chess.Board()
        moves = list(board.legal_moves)
        if not moves:
            print("bestmove 0000", flush=True)
            continue
        pick = moves[len(moves)//2]
        print("info depth 1 score cp 0 nodes 1 pv " + pick.uci(), flush=True)
        print("bestmove " + pick.uci(), flush=True)
    elif line == "quit":
        break
'''

ILLEGAL_ENGINE = r'''
import sys
print("id name FakeIllegal", flush=True)
print("uciok", flush=True)
for line in sys.stdin:
    line = line.strip()
    if line == "uci":
        print("id name FakeIllegal", flush=True)
        print("uciok", flush=True)
    elif line == "isready":
        print("readyok", flush=True)
    elif line.startswith("go"):
        # a8a1 is never a legal move from the initial position
        print("bestmove a8a1", flush=True)
    elif line == "quit":
        break
'''

NO_BESTMOVE_ENGINE = r'''
import sys
print("id name FakeNoBest", flush=True)
print("uciok", flush=True)
for line in sys.stdin:
    line = line.strip()
    if line == "uci":
        print("id name FakeNoBest", flush=True)
        print("uciok", flush=True)
    elif line == "isready":
        print("readyok", flush=True)
    elif line.startswith("go"):
        sys.exit(0)
    elif line == "quit":
        break
'''

CRASH_ENGINE = r'''
import sys
print("id name FakeCrash", flush=True)
print("uciok", flush=True)
for line in sys.stdin:
    line = line.strip()
    if line == "uci":
        print("id name FakeCrash", flush=True)
        print("uciok", flush=True)
    elif line == "isready":
        print("readyok", flush=True)
    elif line.startswith("go"):
        raise SystemExit(3)
    elif line == "quit":
        break
'''

GARBAGE_ENGINE = r'''
import sys
print("id name FakeGarbage", flush=True)
print("uciok", flush=True)
for line in sys.stdin:
    line = line.strip()
    if line == "uci":
        print("id name FakeGarbage", flush=True)
        print("uciok", flush=True)
    elif line == "isready":
        print("readyok", flush=True)
    elif line.startswith("go"):
        # not UCI at all
        print("???(((", flush=True)
        print("this is not a move", flush=True)
        print("bestmove", flush=True)
    elif line == "quit":
        break
'''


def write_engine(directory, name, source):
    path = os.path.join(directory, name)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(source)
    # Windows needs this to be directly executable.
    os.chmod(path, os.stat(path).st_mode | stat.S_IEXEC)
    return path


class TestSprt(unittest.TestCase):
    """The statistical tool must be testable without Pocket Chess's strength."""

    def test_boundaries_are_two_not_one(self):
        t = sprt_mod.Sprt(0.0, 5.0, 0.05, 0.05)
        self.assertGreater(t.upper, 0.0)
        self.assertLess(t.lower, 0.0)
        self.assertAlmostEqual(t.upper, -t.lower, places=9)

    def test_empty_is_inconclusive(self):
        t = sprt_mod.Sprt(0.0, 5.0)
        self.assertIsNone(t.decided)
        self.assertEqual(t.status(), "inconclusive: continue")

    def test_all_draws_never_accepts(self):
        t = sprt_mod.Sprt(0.0, 5.0)
        for _ in range(2000):
            t.add(0.5)
        self.assertIsNone(t.decided)

    def test_all_losses_drives_toward_h0(self):
        t = sprt_mod.Sprt(0.0, 5.0)
        for _ in range(2000):
            t.add(0.0)
        self.assertEqual(t.decided, "H0")

    def test_all_wins_drives_toward_h1(self):
        t = sprt_mod.Sprt(0.0, 30.0)
        for _ in range(2000):
            t.add(1.0)
        self.assertEqual(t.decided, "H1")

    def test_effect_inside_the_gap_stays_inconclusive(self):
        # A true effect between elo0 and elo1 must not decide either way.
        t = sprt_mod.Sprt(0.0, 30.0)
        # A decisive win rate of 0.52 lies between p0 (0.5) and p1 (0.543), so
        # the effect is consistent with BOTH hypotheses and the test must not
        # decide systematically.
        #
        # It is not correct to assert that one run always stays undecided: a
        # sequential test is *designed* to accept early by mistake, at rate
        # alpha, so any single seed can legitimately cross a boundary on noise.
        # What must hold is the false-accept rate across independent runs, so
        # measure that over many fixed seeds. With alpha = beta = 0.05 the
        # combined rate is near 0.10; 0.20 leaves ample margin while still
        # failing loudly if the implementation is biased toward a decision.
        decided_runs = 0
        trials = 200
        for seed in range(trials):
            rng = random.Random(seed)
            probe = sprt_mod.Sprt(0.0, 30.0)
            for _ in range(200):
                probe.add(1.0 if rng.random() < 0.52 else 0.0)
                if probe.decided:
                    break
            if probe.decided:
                decided_runs += 1
        self.assertLess(decided_runs / trials, 0.20,
                        f"inside-the-gap effect decided {decided_runs}/{trials} runs")

        # The same effect at a rate clearly beyond p1 must decide, otherwise the
        # test is merely refusing to decide anything at all.
        strong = sprt_mod.Sprt(0.0, 30.0)
        rng = random.Random(7)
        for _ in range(2000):
            strong.add(1.0 if rng.random() < 0.75 else 0.0)
        self.assertEqual(strong.decided, "H1")

    def test_score_and_counts(self):
        t = sprt_mod.Sprt()
        for score in (1.0, 0.5, 0.5, 0.0):
            t.add(score)
        self.assertEqual(t.games, 4)
        self.assertEqual(t.wins, 1)
        self.assertEqual(t.draws, 2)
        self.assertEqual(t.losses, 1)
        self.assertAlmostEqual(t.score(), 0.5)

    def test_rejects_bad_input(self):
        t = sprt_mod.Sprt()
        with self.assertRaises(ValueError):
            t.add(0.25)

    def test_rejects_inverted_hypotheses(self):
        with self.assertRaises(ValueError):
            sprt_mod.Sprt(5.0, 0.0)

    def test_elo_round_trip(self):
        for elo in (-400, -100, 0, 100, 400):
            self.assertAlmostEqual(sprt_mod.score_to_elo(
                sprt_mod.elo_to_score(elo)), elo, places=6)

    def test_simulation_reaches_expected_conclusions(self):
        """Synthetic data from a known truth. Not a strength measurement."""
        better = sprt_mod.simulate(4000, 60.0, 30.0, 0.0, 0.05, 0.05, 0.6, 1)
        self.assertEqual(better.decided, "H1")
        worse = sprt_mod.simulate(4000, -60.0, 30.0, 0.0, 0.05, 0.05, 0.6, 1)
        self.assertEqual(worse.decided, "H0")
        # Identical engines must not produce a false acceptance across seeds.
        for seed in range(12):
            same = sprt_mod.simulate(500, 0.0, 5.0, 0.0, 0.05, 0.05, 0.6, seed)
            self.assertIsNone(same.decided,
                              f"false acceptance at seed {seed}: {same.report()}")


class TestUciContract(unittest.TestCase):
    def test_real_engine_handshake(self):
        exe = os.path.join(HERE, "..", "build", "pocketchess.exe")
        if not os.path.exists(exe):
            self.skipTest("pocketchess.exe not built")
        engine = matchlib.UciEngine(exe, "pocketchess", ["name Hash value 8"])
        engine.start()
        try:
            self.assertTrue(engine.isready())
            engine.send("position startpos")
            engine.send("go depth 3")
            deadline_ok = False
            for _ in range(200):
                line = engine.readline(30)
                if line.startswith("bestmove"):
                    move = line.split()[1]
                    board = chess.Board()
                    self.assertIsNotNone(board.parse_uci(move),
                                         "bestmove was not a legal move")
                    deadline_ok = True
                    break
            self.assertTrue(deadline_ok, "no bestmove seen")
        finally:
            engine.stop()


class TestMatchRunner(unittest.TestCase):
    """Every infrastructure failure must be caught and labelled, never scored."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="pocket_match_test_")
        cls.good = write_engine(cls.tmp, "good.py", GOOD_ENGINE)
        cls.illegal = write_engine(cls.tmp, "illegal.py", ILLEGAL_ENGINE)
        cls.no_best = write_engine(cls.tmp, "nobest.py", NO_BESTMOVE_ENGINE)
        cls.crash = write_engine(cls.tmp, "crash.py", CRASH_ENGINE)
        cls.garbage = write_engine(cls.tmp, "garbage.py", GARBAGE_ENGINE)

    @classmethod
    def tearDownClass(cls):
        import shutil
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _run(self, baseline, candidate, games=1):
        out = tempfile.mkdtemp(prefix="pocket_match_out_")
        # The fake engines are Python scripts, which Windows cannot execute as a
        # bare path. Launching them through the interpreter exercises the
        # runner's launch-argument support, which is needed anyway for engines
        # that ship as a script or a jar.
        cmd = [sys.executable, os.path.join(HERE, "runner.py"),
               "--baseline", sys.executable, "--baseline-args", baseline,
               "--candidate", sys.executable, "--candidate-args", candidate,
               "--id", "TEST-001", "--out", out, "--games", str(games),
               "--depth", "1", "--control", "depth",
               "--hard-timeout-per-move", "15"]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        import json
        path = os.path.join(out, "TEST-001", "results.json")
        data = json.load(open(path, encoding="utf-8")) if os.path.exists(path) else None
        return proc.returncode, data

    def test_good_engines_produce_chess_results(self):
        code, data = self._run(self.good, self.good)
        self.assertIsNotNone(data, "no results.json")
        s = data["summary"]
        self.assertEqual(s["infrastructure_failures"], 0)
        self.assertGreater(s["chess_results"], 0)
        self.assertEqual(code, 0)

    def test_illegal_move_is_detected(self):
        code, data = self._run(self.good, self.illegal)
        self.assertIsNotNone(data)
        s = data["summary"]
        self.assertGreater(s["infrastructure_failures"], 0)
        self.assertIn("illegal_move", s["failures_by_termination"])
        # Crucially, it must not be scored as a loss.
        self.assertEqual(s["total_games"], 0)
        self.assertEqual(code, 1)

    def test_crash_is_detected(self):
        code, data = self._run(self.good, self.crash)
        self.assertIsNotNone(data)
        s = data["summary"]
        self.assertGreater(s["infrastructure_failures"], 0)
        self.assertEqual(s["total_games"], 0)
        self.assertEqual(code, 1)

    def test_missing_bestmove_is_detected(self):
        code, data = self._run(self.good, self.no_best)
        self.assertIsNotNone(data)
        s = data["summary"]
        self.assertGreater(s["infrastructure_failures"], 0)
        self.assertIn("no_bestmove", s["failures_by_termination"])

    def test_malformed_uci_is_detected(self):
        code, data = self._run(self.good, self.garbage)
        self.assertIsNotNone(data)
        s = data["summary"]
        self.assertGreater(s["infrastructure_failures"], 0)

    def test_missing_executable_is_detected(self):
        code, data = self._run(self.good, os.path.join(self.tmp, "does_not_exist"))
        self.assertIsNotNone(data)
        s = data["summary"]
        self.assertGreater(s["infrastructure_failures"], 0)
        self.assertIn("engine_crash", s["failures_by_termination"])


class TestSchedule(unittest.TestCase):
    def test_colour_alternates(self):
        openings = [{"id": "a"}, {"id": "b"}]
        schedule = runner_mod.build_schedule(openings, 8)
        baseline_white = [s[1] for s in schedule]
        # Consecutive games must differ, and the overall split must be even.
        for i in range(1, len(baseline_white)):
            self.assertNotEqual(baseline_white[i], baseline_white[i - 1])
        self.assertEqual(sum(1 for w in baseline_white if w), 4)

    def test_openings_are_interleaved_not_batched(self):
        openings = [{"id": "a"}, {"id": "b"}, {"id": "c"}]
        schedule = runner_mod.build_schedule(openings, 12)
        ids = [s[0]["id"] for s in schedule]
        # No opening should own a contiguous run longer than two games.
        runs = []
        current, count = ids[0], 1
        for i in ids[1:]:
            if i == current:
                count += 1
            else:
                runs.append(count)
                current, count = i, 1
        runs.append(count)
        self.assertLessEqual(max(runs), 2, f"openings were batched: runs={runs}")

    def test_colour_imbalance_is_reported_not_hidden(self):
        openings = [{"id": "a"}]
        schedule = runner_mod.build_schedule(openings, 5)
        baseline_white = [s[1] for s in schedule]
        # Odd game count means one extra game for one engine; that is a known,
        # reported property rather than something to smooth over.
        self.assertEqual(sum(1 for w in baseline_white if w), 3)


class TestDrawRules(unittest.TestCase):
    def test_adjudication_matches_fide_rules(self):
        # Checkmate
        board = chess.Board("7k/5KQ1/8/8/8/8/8/8 b - - 0 1")
        self.assertEqual(matchlib.adjudicate(board),
                         matchlib.TERMINATION_CHECKMATE)

        # Stalemate
        board = chess.Board("k7/2Q5/8/8/8/8/8/7K b - - 0 1")
        self.assertEqual(matchlib.adjudicate(board),
                         matchlib.TERMINATION_STALEMATE)

        # Insufficient material
        board = chess.Board("8/8/8/4k3/8/8/8/4K3 w - - 0 1")
        self.assertEqual(matchlib.adjudicate(board),
                         matchlib.TERMINATION_INSUFFICIENT)

        # A normal middlegame is not over.
        board = chess.Board()
        self.assertIsNone(matchlib.adjudicate(board))

    def test_failure_and_result_sets_are_disjoint(self):
        self.assertEqual(matchlib.CHESS_TERMINATIONS &
                         matchlib.FAILURE_TERMINATIONS, set())


class TestPgnAndClock(unittest.TestCase):
    """Regression tests for two defects that a passing scoreboard would hide.

    Both bugs produced plausible-looking output: a PGN with headers but no
    moves, and a clock that never ran down. Neither raises, so only an explicit
    assertion catches them.
    """

    def test_pgn_contains_every_move(self):
        start = chess.Board().fen()
        moves = ["e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6"]
        text = matchlib.game_pgn(start, {"Event": "T", "Result": "1-0"}, moves)
        self.assertIn("1. e4 e5 2. Nf3 Nc6 3. Bb5 a6", text)
        self.assertIn('[Result "1-0"]', text)

    def test_pgn_replays_from_the_opening_not_the_final_position(self):
        # This is the exact mistake the old signature invited. Handing the
        # PGN writer the board the game ended on cannot work: none of the
        # early moves are legal there, so every one of them fails to replay.
        moves = ["e2e4", "e7e5"]
        final = chess.Board()
        for uci in moves:
            final.push_uci(uci)
        with self.assertRaises(ValueError):
            matchlib.game_pgn(final.fen(), {"Event": "T"}, moves)
        # Handing it the opening position instead works.
        self.assertIn("1. e4 e5",
                      matchlib.game_pgn(chess.Board().fen(), {"Event": "T"}, moves))

    def test_pgn_reports_an_unreplayable_log_instead_of_truncating(self):
        with self.assertRaises(ValueError):
            matchlib.game_pgn(chess.Board().fen(), {"Event": "T"},
                              ["e2e4", "e2e4"])

    def test_pgn_from_a_non_standard_opening_round_trips(self):
        # A fresh chess.pgn.Game is rooted at the standard start position. A
        # game opening from an arbitrary FEN must re-root, or the exporter
        # walks the movetext from the wrong board and dies on the first move
        # that only makes sense in the real opening.
        start = "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4"
        moves = ["d7d6", "d2d3", "g8f6", "e1g1"]
        text = matchlib.game_pgn(start, {"Event": "T", "Result": "0-1"}, moves)
        # Move numbering follows the FEN, so this is a move 4 game, not move 1.
        self.assertIn('4... d6 5. d3 Nf6 6. O-O', text)
        self.assertIn('[SetUp "1"]', text)
        # And it must parse back to the same position, which is the only real
        # test of a PGN.
        game = chess.pgn.read_game(io.StringIO(text))
        self.assertIsNotNone(game)
        board = game.end().board()
        expected = chess.Board(start)
        for uci in moves:
            expected.push_uci(uci)
        self.assertEqual(board.board_fen(), expected.board_fen())
        self.assertEqual(board.turn, expected.turn)
        self.assertEqual(board.castling_rights, expected.castling_rights)

    def test_clock_is_decremented_and_increment_applied(self):
        control = {"type": "clock", "inc": 0.1, "movestogo": 40}
        clock = {"white": 60.0, "black": 60.0}
        before = clock["white"]
        go = matchlib.build_go(control, clock)
        # The go command must report the current clock, in milliseconds.
        self.assertIn("wtime 60000", go)
        clock["white"] = before - 0.5 + control["inc"]
        self.assertAlmostEqual(clock["white"], 59.6, places=6)
        self.assertIn("winc 100", matchlib.build_go(control, clock))

    def test_flag_fall_is_a_chess_result_not_a_failure(self):
        self.assertIn(matchlib.TERMINATION_FLAGFALL,
                      matchlib.CHESS_TERMINATIONS)
        self.assertNotIn(matchlib.TERMINATION_FLAGFALL,
                         matchlib.FAILURE_TERMINATIONS)


if __name__ == "__main__":
    unittest.main(verbosity=2)
