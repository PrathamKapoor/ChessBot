#!/usr/bin/env python3
"""UCI process wrapper and single-game play for Pocket Chess match experiments.

This is deliberately first-party and deliberately small. See runner.py's module
docstring for why, and RESEARCH.md for the decision record.

What this file is responsible for:
  * starting an engine process and completing the UCI handshake
  * playing one complete game to a result, or to a recorded failure
  * classifying every way a game can end, so that an infrastructure failure is
    never silently reported as a chess result
  * writing a valid PGN and a structured record for the game

What this file is NOT responsible for, and deliberately so:
  * adjudicating with endgame tablebases
  * adjudication engines beyond python-chess's own rule implementation
  * anything statistical; see sprt.py

DRAW MODEL, AND WHERE IT DIFFERS FROM THE ENGINE
-------------------------------------------------
The runner follows the FIDE rules: threefold repetition, fifty-move rule, and
insufficient material as python-chess defines them. The ENGINE does not. Inside its
search the engine treats a twofold repetition as a draw, which is stronger than
the rule, and its insufficient-material test does not distinguish the
K+B vs K+B same-colour case. Both divergences are conservative - the engine
avoids repetition lines slightly earlier than the rules require - but they mean
the engine's internal draw model and the runner's adjudication model are not
identical. This is recorded rather than hidden.
"""
import datetime
import os
import subprocess
import time

try:
    import chess
    import chess.pgn
except ImportError:  # pragma: no cover
    chess = None

# Every distinct way a game can end. Only the first four are chess results.
TERMINATION_CHECKMATE = "checkmate"
TERMINATION_STALEMATE = "stalemate"
TERMINATION_THREEFOLD = "threefold_repetition"
TERMINATION_FIFTY = "fifty_move_rule"
TERMINATION_INSUFFICIENT = "insufficient_material"
TERMINATION_MOVELIMIT = "move_limit_reached"
TERMINATION_ILLEGAL = "illegal_move"
TERMINATION_NO_BESTMOVE = "no_bestmove"
TERMINATION_TIMEOUT = "timeout"
TERMINATION_CRASH = "engine_crash"
TERMINATION_PROTOCOL = "protocol_violation"
TERMINATION_DISCONNECTED = "engine_disconnected"
TERMINATION_FLAGFALL = "flag_fall"

CHESS_TERMINATIONS = {
    TERMINATION_CHECKMATE,
    TERMINATION_STALEMATE,
    TERMINATION_THREEFOLD,
    TERMINATION_FIFTY,
    TERMINATION_INSUFFICIENT,
    TERMINATION_MOVELIMIT,
    TERMINATION_FLAGFALL,
}

# Failures are kept separate on purpose. Part 11 of the phase requires that an
# infrastructure failure be distinguishable from a chess result, and the only
# honest way to do that is to refuse to conflate them in the data model.
FAILURE_TERMINATIONS = {
    TERMINATION_ILLEGAL,
    TERMINATION_NO_BESTMOVE,
    TERMINATION_TIMEOUT,
    TERMINATION_CRASH,
    TERMINATION_PROTOCOL,
    TERMINATION_DISCONNECTED,
}


class EngineError(Exception):
    """An engine misbehaved. Carries the termination reason for the record."""

    def __init__(self, message, termination):
        super().__init__(message)
        self.termination = termination


class UciEngine:
    """One engine subprocess speaking UCI on stdin/stdout.

    `args` are leading arguments after the executable. This is not decoration:
    many engines ship as a script or a jar and cannot be launched as a bare path,
    so the runner needs to be able to say "python engine.py" or "java -jar
    engine.jar". The self-test also relies on it to drive deliberately broken
    fake engines.
    """

    def __init__(self, path, name, options=None, log=None, args=None):
        self.path = path
        self.args = list(args) if args else []
        self.name = name
        self.options = options or []
        self.log = log
        self.proc = None

    # -- lifecycle ---------------------------------------------------------
    def start(self):
        if not os.path.exists(self.path):
            raise EngineError(f"{self.name}: no such executable {self.path}",
                              TERMINATION_CRASH)
        try:
            self.proc = subprocess.Popen(
                [self.path] + self.args, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                bufsize=1,
            )
        except OSError as exc:
            raise EngineError(f"{self.name}: cannot start: {exc}", TERMINATION_CRASH) from exc

        self._expect_uciok()
        for opt in self.options:
            self.send(f"setoption {opt}")
        self.isready()

    def stop(self):
        if self.proc is None:
            return
        try:
            self.send("quit")
            self.proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            pass
        finally:
            if self.proc.poll() is None:
                self.proc.kill()
            self.proc = None

    # -- protocol ----------------------------------------------------------
    def send(self, line):
        if self.proc is None or self.proc.poll() is not None:
            raise EngineError(f"{self.name}: not running", TERMINATION_CRASH)
        if self.log is not None:
            self.log.append(f"-> {self.name}: {line}")
        try:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError) as exc:
            raise EngineError(f"{self.name}: write failed: {exc}",
                              TERMINATION_DISCONNECTED) from exc

    def readline(self, timeout):
        """Read one line, or raise. A closed pipe is a crash, not an empty line."""
        try:
            line = self.proc.stdout.readline()
        except (OSError, ValueError) as exc:
            raise EngineError(f"{self.name}: read failed: {exc}",
                              TERMINATION_CRASH) from exc
        if line == "":
            # EOF on the pipe does not mean the exit status is known yet:
            # poll() can still return None at the instant the pipe closes.
            # Wait briefly so the real code is available, otherwise an engine
            # that exits cleanly without answering is indistinguishable from
            # one that crashed.
            try:
                code = self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                code = self.proc.poll()
            # A clean exit while a search was outstanding is an engine declining
            # to answer, which is a different fault from dying. Separating them
            # makes the failure record actionable rather than just "crashed".
            if code == 0 and self.expecting_bestmove:
                raise EngineError(f"{self.name}: exited without a bestmove",
                                  TERMINATION_NO_BESTMOVE)
            raise EngineError(
                f"{self.name}: process ended (exit {code})",
                TERMINATION_CRASH)
        line = line.strip()
        if self.log is not None:
            self.log.append(f"<- {self.name}: {line}")
        return line

    def _expect_uciok(self):
        self.send("uci")
        deadline = time.time() + 20
        while time.time() < deadline:
            line = self.readline(20)
            if line == "uciok":
                return
        raise EngineError(f"{self.name}: no uciok within 20s", TERMINATION_PROTOCOL)

    def isready(self):
        self.send("isready")
        deadline = time.time() + 20
        while time.time() < deadline:
            line = self.readline(20)
            if line == "readyok":
                return True
        raise EngineError(f"{self.name}: no readyok within 20s", TERMINATION_PROTOCOL)

    def bestmove(self, go_args, hard_timeout):
        """Issue a go and read until bestmove. Returns (move, info_lines).

        `go_args` is the argument body from build_go, for example "depth 6".
        `hard_timeout` is the runner's own ceiling in seconds and is
        deliberately a separate parameter: conflating the two once made the
        engine receive "go 15" instead of "go depth 6".
        """
        self.expecting_bestmove = True
        self.send("go " + go_args)
        deadline = time.time() + hard_timeout
        infos = []
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                # Ask it to stop and give it a grace period rather than killing
                # it: an engine interrupted mid-search still owes a bestmove.
                self._stop_and_collect(infos, grace=10)
                raise EngineError(f"{self.name}: exceeded its own time budget",
                                  TERMINATION_TIMEOUT)
            line = self.readline(remaining)
            if line.startswith("bestmove"):
                self.expecting_bestmove = False
                parts = line.split()
                if len(parts) < 2:
                    raise EngineError(f"{self.name}: malformed bestmove: {line!r}",
                                      TERMINATION_NO_BESTMOVE)
                return parts[1], infos
            if line.startswith("info"):
                infos.append(line)

    def _stop_and_collect(self, infos, grace):
        try:
            self.send("stop")
        except EngineError:
            return
        deadline = time.time() + grace
        while time.time() < deadline:
            try:
                line = self.readline(max(0.1, deadline - time.time()))
            except EngineError:
                return
            if line.startswith("bestmove"):
                self.expecting_bestmove = False
                return
            if line.startswith("info"):
                infos.append(line)


def position_command(start_fen, move_log):
    """Build the UCI `position` command for the current line of play.

    The move list is replayed on top of the opening FEN rather than sending a
    bare current-position FEN. A FEN cannot express the full history, so
    replaying is what preserves castling rights, the en passant square and the
    repetition count that the engine needs to adjudicate draws identically.
    Sending only `position fen ...` would silently break the engine's own
    twofold-repetition and fifty-move logic.
    """
    if move_log:
        return "position fen " + start_fen + " moves " + " ".join(move_log)
    return "position fen " + start_fen


def build_go(control, clock):
    """Build the go argument list for the configured control."""
    if control["type"] == "depth":
        return f"depth {control['depth']}"
    if control["type"] == "movetime":
        return f"movetime {control['movetime_ms']}"
    if control["type"] == "clock":
        white_ms = int(clock["white"] * 1000)
        black_ms = int(clock["black"] * 1000)
        inc = int(control["inc"] * 1000)
        return (f"wtime {white_ms} btime {black_ms} "
                f"winc {inc} binc {inc} movestogo {control['movestogo']}")
    raise ValueError(f"unknown control type {control['type']!r}")


def adjudicate(board):
    """Return a termination string if the game is over under the rules, else None."""
    if board.is_checkmate():
        return TERMINATION_CHECKMATE
    if board.is_stalemate():
        return TERMINATION_STALEMATE
    if board.is_insufficient_material():
        return TERMINATION_INSUFFICIENT
    if board.can_claim_threefold_repetition():
        return TERMINATION_THREEFOLD
    if board.can_claim_fifty_moves():
        return TERMINATION_FIFTY
    return None


def play_game(board, white_engine, black_engine, control, clock, max_plies,
              hard_timeout_per_move, move_log):
    """Play from `board` to completion. Mutates board.

    Returns (result, termination, plies).
      result is one of "1-0", "0-1", "1/2-1/2" or None when the game did not
      finish as a chess result.
    """
    # Captured before any push, so every position command can be rebuilt from
    # the opening FEN plus the moves played since.
    start_fen = board.fen()
    while True:
        term = adjudicate(board)
        if term:
            return result_for(board), term, board.ply()

        if board.ply() >= max_plies:
            # A draw by adjudication is a real chess outcome and is recorded as
            # one, distinct from any infrastructure failure.
            return "1/2-1/2", TERMINATION_MOVELIMIT, board.ply()

        white_to_move = board.turn == chess.WHITE
        engine = white_engine if white_to_move else black_engine

        engine.send(position_command(start_fen, move_log))
        go = build_go(control, clock)

        # Time must be charged to the side to move only after the search has
        # finished, and only for the part actually spent thinking. Passing the
        # un-decremented clock on every move, as an earlier version did, means
        # the clock never runs down and a timed match silently degrades into
        # an unlimited one. Increment is added after the deduction, matching
        # the rules: you gain it for having moved.
        move_started = time.time()
        move_str, _infos = engine.bestmove(go, hard_timeout_per_move)
        spent = time.time() - move_started

        if control["type"] == "clock":
            side = "white" if white_to_move else "black"
            remaining = clock[side] - spent
            if remaining <= 0:
                # Running out of time is a loss on time: a chess outcome, not a
                # runner fault. White flagging means white loses.
                return ("0-1" if white_to_move else "1-0",
                        TERMINATION_FLAGFALL, board.ply())
            clock[side] = remaining
            if control.get("inc"):
                clock[side] += control["inc"]

        if move_str in ("(none)", "0000"):
            # No move offered. If the position is genuinely terminal that is a
            # chess result; otherwise it is the engine failing to answer.
            term = adjudicate(board)
            if term:
                return result_for(board), term, board.ply()
            raise EngineError(f"{engine.name}: bestmove 0000 in a playable position",
                              TERMINATION_NO_BESTMOVE)

        move = board.parse_uci(move_str)  # raises ValueError if malformed
        move_log.append(move.uci())
        board.push(move)


def result_for(board):
    outcome = board.outcome(claim_draw=False) if False else None
    if board.is_checkmate():
        return "0-1" if board.turn == chess.WHITE else "1-0"
    return "1/2-1/2"


def game_pgn(start_fen, headers, move_log):
    """Build a PGN Game for a completed game.

    The moves are replayed on a *fresh* board built from the opening position.
    Replaying them on the board the game actually ended on is wrong: that board
    is already at the final position, so almost every move in the log is
    illegal there and the loop bails out immediately, producing a PGN with a
    header and no moves.
    """
    import io
    game = chess.pgn.Game()
    for key, value in headers.items():
        game.headers[key] = value

    replay = chess.Board(start_fen)
    node = game
    for uci in move_log:
        try:
            move = replay.parse_uci(uci)
        except ValueError:
            # A move that cannot be replayed means the log and the recorded
            # position disagree, which is a bug worth surfacing rather than
            # quietly truncating the game record.
            raise ValueError(
                f"cannot replay {uci!r} from {replay.fen()}") from None
        node = node.add_variation(move)
        replay.push(move)

    out = io.StringIO()
    exporter = chess.pgn.FileExporter(out)
    game.accept(exporter)
    return out.getvalue()
