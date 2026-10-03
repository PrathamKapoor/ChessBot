#!/usr/bin/env python3
"""First-party match runner for Pocket Chess A/B experiments.

WHY FIRST-PARTY RATHER THAN A MATURE EXTERNAL RUNNER
-----------------------------------------------------
The alternatives were fastchess and cutechess. Neither is installed here, and
vendoring either would add a large codebase whose licence would have to be
examined and reconciled with this repository before it could be used. The
experiment set this phase needs is small and specific: two engines, one opening
suite, fixed-depth or a single clock control, colour balancing, and machine-
readable output that records enough provenance to reconstruct the match.

This runner does exactly that and nothing more. It deliberately does NOT
implement endgame tablebase adjudication, adjudication engines beyond the FIDE
rules, cluster play, EPD/arena modes, or contempt. Those are real features of
mature runners and their absence is recorded as a limitation rather than papered
over.

BATCHING AND COLOUR
-------------------
Games are emitted as an interleaved schedule rather than as "all of A, then all
of B". Each opening contributes an even/odd pair so that consecutive games always
swap which engine has White, and openings are visited in order. This means a
thermal or background-load drift during a long match affects both engines roughly
equally instead of penalising whichever played second.

SCHEDULE SHAPE, EXPLICITLY
--------------------------
For N games over openings O1..Ok with `games` total, game index g uses opening
O[(g // 2) mod k] and gives White to the baseline when g is even and to the
candidate when g is odd. With an even number of games per opening this gives an
exact colour balance; with an odd count the imbalance is one game and is reported.

USAGE
-----
    py -3 match/runner.py --baseline build/pocketchess.exe \\
        --candidate build/pocketchess_candidate.exe \\
        --suite match/suite_core-01.txt --games 24 --depth 4 \\
        --id P3C-SELF-001 --out match/results/P3C-SELF-001
"""
import argparse
import datetime
import json
import os
import platform
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import matchlib
from matchlib import EngineError, UciEngine, CHESS_TERMINATIONS, FAILURE_TERMINATIONS
import sprt as sprt_mod

try:
    import chess
    import chess.pgn
except ImportError:
    print("python-chess is required", file=sys.stderr)
    sys.exit(2)


def load_suite(path):
    """Read a suite file. Same format as verify_openings.py, kept simple here."""
    openings = []
    with open(path, "r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#") or line.startswith("moves:"):
                continue
            parts = line.split()
            if len(parts) < 4 or parts[1].lower() == "plies":
                continue
            openings.append({
                "id": parts[0],
                "plies": int(parts[1]),
                "structure": parts[2],
                "fen": " ".join(parts[3:]),
            })
    if not openings:
        raise ValueError(f"no openings parsed from {path}")
    return openings


def build_schedule(openings, games):
    """Interleaved (opening, baseline_is_white) schedule. See module docstring."""
    schedule = []
    n = len(openings)
    for g in range(games):
        opening = openings[(g // 2) % n]
        baseline_white = (g % 2) == 0
        schedule.append((opening, baseline_white, g))
    return schedule


def git_commit(path):
    try:
        out = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"],
                             capture_output=True, text=True, timeout=15)
        if out.returncode == 0:
            return out.stdout.strip()
    except Exception:  # noqa: BLE001
        pass
    return None


def git_tree_state(path, ignore_dirs=()):
    """Whether the working tree is clean, and which paths differ.

    A commit hash alone is not provenance when the tree is dirty: the binary
    under test may have been built from changes that were never committed, and
    the record would then look reproducible while nothing about it can be
    rebuilt. Recording the dirtiness keeps that visible instead of implied.

    The runner's own output directory is excluded. A match always writes its
    results, so without this every run would report itself as the thing that
    dirtied the tree, which would make the signal useless in practice.
    """
    ignored = tuple(d.replace("\\", "/").rstrip("/") for d in ignore_dirs)
    try:
        out = subprocess.run(["git", "-C", path, "status", "--porcelain"],
                             capture_output=True, text=True, timeout=30)
        if out.returncode == 0:
            changed = []
            for line in out.stdout.splitlines():
                if not line.strip():
                    continue
                name = line[3:].strip().strip('"').replace("\\", "/")
                if any(name == i or name.startswith(i + "/") for i in ignored):
                    continue
                changed.append(name)
            return {"clean": not changed, "changed_count": len(changed),
                    "changed": changed[:20]}
    except Exception:  # noqa: BLE001
        pass
    return None


def engine_fingerprint(path):
    """Identity of an engine binary for the record: path, size, mtime, sha256."""
    import hashlib
    info = {"path": os.path.abspath(path)}
    try:
        stat = os.stat(path)
        info["size_bytes"] = stat.st_size
        info["mtime"] = datetime.datetime.fromtimestamp(
        stat.st_mtime, datetime.timezone.utc).isoformat()
        digest = hashlib.sha256()
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
        info["sha256"] = digest.hexdigest()
    except OSError as exc:
        info["error"] = str(exc)
    return info


def run_one_game(cfg, opening, baseline_white, game_id, pgn_dir, verbose=False):
    """Play one game and return its record. Never raises for engine misbehaviour."""
    board = chess.Board(opening["fen"])
    clock = {"white": cfg["base_seconds"], "black": cfg["base_seconds"]}
    move_log = []
    log = [] if verbose else None

    white_path = cfg["baseline"] if baseline_white else cfg["candidate"]
    black_path = cfg["candidate"] if baseline_white else cfg["baseline"]
    white_name = cfg["baseline_name"] if baseline_white else cfg["candidate_name"]
    black_name = cfg["candidate_name"] if baseline_white else cfg["baseline_name"]

    white_engine = UciEngine(white_path, white_name, cfg["options"], log,
                             cfg["launch_args"].get(white_name))
    black_engine = UciEngine(black_path, black_name, cfg["options"], log,
                             cfg["launch_args"].get(black_name))

    termination = None
    result = None
    plies = 0
    error = None
    started = time.time()

    try:
        white_engine.start()
        black_engine.start()
        white_engine.isready()
        black_engine.isready()
        result, termination, plies = matchlib.play_game(
            board, white_engine, black_engine, cfg["control"], clock,
            cfg["max_plies"], cfg["hard_timeout_per_move"], move_log)
    except EngineError as exc:
        termination = exc.termination
        error = str(exc)
    except ValueError as exc:
        # A malformed move string from an engine is an illegal move, not a crash.
        termination = matchlib.TERMINATION_ILLEGAL
        error = f"malformed move: {exc}"
    except Exception as exc:  # noqa: BLE001
        termination = matchlib.TERMINATION_CRASH
        error = f"unexpected: {type(exc).__name__}: {exc}"
    finally:
        white_engine.stop()
        black_engine.stop()

    elapsed = time.time() - started

    # The candidate's point of view, which is what the SPRT consumes.
    if result == "1/2-1/2":
        candidate_score = 0.5
    elif result in ("1-0", "0-1"):
        candidate_is_white = not baseline_white
        candidate_won = (result == "1-0") == candidate_is_white
        candidate_score = 1.0 if candidate_won else 0.0
    else:
        # No chess result: an infrastructure failure. Recorded as None so it can
        # never be silently averaged into a score.
        candidate_score = None

    record = {
        "experiment_id": cfg["experiment_id"],
        "game_id": game_id,
        "opening_id": opening["id"],
        "opening_structure": opening["structure"],
        "opening_plies": opening["plies"],
        "baseline_engine": cfg["baseline_name"],
        "candidate_engine": cfg["candidate_name"],
        "baseline_white": baseline_white,
        "white_engine": white_name,
        "black_engine": black_name,
        "baseline_commit": cfg["baseline_commit"],
        "candidate_commit": cfg["candidate_commit"],
        "control": cfg["control_desc"],
        "hash_mb": cfg["hash_mb"],
        "threads": 1,
        "result": result,
        "termination": termination,
        "candidate_score": candidate_score,
        "plies": plies,
        "moves_played": len(move_log),
        "elapsed_s": round(elapsed, 3),
        "is_chess_result": termination in CHESS_TERMINATIONS,
        "is_failure": termination in FAILURE_TERMINATIONS,
        "error": error,
    }

    if result is not None:
        headers = {
            "Event": cfg["experiment_id"],
            "Site": "PocketChess local match",
            "Date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y.%m.%d"),
            "Round": str(game_id),
            "White": white_name,
            "Black": black_name,
            "Result": result,
            "Opening": opening["id"],
        }
        # Writing the PGN must not be able to destroy a game that was played and
        # adjudicated correctly. A failure here means the move log and the
        # recorded position disagree, which is worth reporting loudly, but the
        # chess result itself is still valid and still belongs in the record.
        try:
            pgn_text = matchlib.game_pgn(opening["fen"], headers, move_log)
        except Exception as exc:  # noqa: BLE001
            record["pgn_error"] = f"{type(exc).__name__}: {exc}"
            print(f"  game {game_id}: PGN could not be written "
                  f"({type(exc).__name__}: {exc}); result retained", file=sys.stderr)
        else:
            os.makedirs(pgn_dir, exist_ok=True)
            pgn_path = os.path.join(
                pgn_dir, f"{cfg['experiment_id']}-{game_id:04d}.pgn")
            with open(pgn_path, "w", encoding="utf-8") as handle:
                handle.write(pgn_text)
            record["pgn"] = os.path.basename(pgn_path)

    return record


def main():
    ap = argparse.ArgumentParser(description="Pocket Chess A/B match runner")
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--candidate", required=True)
    ap.add_argument("--baseline-name", default="baseline")
    ap.add_argument("--candidate-name", default="candidate")
    ap.add_argument("--baseline-commit", default=None,
                    help="commit the baseline binary was built from "
                         "(default: current HEAD)")
    ap.add_argument("--candidate-commit", default=None,
                    help="commit the candidate binary was built from. Required "
                         "when the candidate differs from HEAD, otherwise the "
                         "provenance record would silently claim the wrong one.")
    ap.add_argument("--baseline-build", default=None,
                    help="free-form build description for the baseline, e.g. "
                         "'cmake Release /O2 MSVC 19.40'")
    ap.add_argument("--candidate-build", default=None,
                    help="free-form build description for the candidate")
    ap.add_argument("--suite", default="match/suite_core-01.txt")
    ap.add_argument("--id", dest="experiment_id", required=True,
                    help="stable experiment identifier, e.g. P3C-SELF-001")
    ap.add_argument("--out", default=None, help="output directory")
    ap.add_argument("--games", type=int, default=12)
    ap.add_argument("--control", choices=["depth", "movetime", "clock"],
                    default="depth")
    ap.add_argument("--depth", type=int, default=4)
    ap.add_argument("--movetime-ms", type=int, default=100)
    ap.add_argument("--base-seconds", type=float, default=10.0)
    ap.add_argument("--inc-seconds", type=float, default=0.1)
    ap.add_argument("--movestogo", type=int, default=40)
    ap.add_argument("--hash-mb", type=int, default=16)
    ap.add_argument("--max-plies", type=int, default=300)
    ap.add_argument("--hard-timeout-per-move", type=float, default=60.0)
    ap.add_argument("--baseline-args", default="",
                    help="extra launch arguments for the baseline, e.g. a script or jar")
    ap.add_argument("--candidate-args", default="",
                    help="extra launch arguments for the candidate")
    ap.add_argument("--setoption", action="append", default=[],
                    help="engine option, e.g. 'name Hash value 16'")
    ap.add_argument("--elo0", type=float, default=0.0)
    ap.add_argument("--elo1", type=float, default=5.0)
    ap.add_argument("--alpha", type=float, default=0.05)
    ap.add_argument("--beta", type=float, default=0.05)
    ap.add_argument("--repo", default=".", help="repository root, for git metadata")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    openings = load_suite(args.suite)
    # Always nest under the experiment id, even when --out is given. An earlier
    # version wrote results.json directly into --out, so running two experiments
    # into the same directory silently overwrote the first one's record. The
    # experiment id is what makes a record identifiable, so it belongs in the
    # path rather than only inside the file.
    root_dir = args.out or os.path.join("match", "results")
    out_dir = os.path.join(root_dir, args.experiment_id)
    os.makedirs(out_dir, exist_ok=True)
    pgn_dir = os.path.join(out_dir, "pgn")

    control = {"type": args.control}
    control_desc = args.control
    if args.control == "depth":
        control["depth"] = args.depth
        control_desc = f"depth {args.depth}"
    elif args.control == "movetime":
        control["movetime_ms"] = args.movetime_ms
        control_desc = f"movetime {args.movetime_ms}ms"
    else:
        control["inc"] = args.inc_seconds
        control["movestogo"] = args.movestogo
        control_desc = (f"clock {args.base_seconds}s+{args.inc_seconds}s "
                        f"mtg {args.movestogo}")

    options = list(args.setoption) + [f"name Hash value {args.hash_mb}"]

    cfg = {
        "baseline": args.baseline,
        "candidate": args.candidate,
        "baseline_name": args.baseline_name,
        "candidate_name": args.candidate_name,
        "experiment_id": args.experiment_id,
        "control": control,
        "control_desc": control_desc,
        "base_seconds": args.base_seconds,
        "hash_mb": args.hash_mb,
        "options": options,
        "max_plies": args.max_plies,
        "hard_timeout_per_move": args.hard_timeout_per_move,
        "launch_args": {
            args.baseline_name: args.baseline_args.split(),
            args.candidate_name: args.candidate_args.split(),
        },
        "baseline_commit": args.baseline_commit or git_commit(args.repo),
        "candidate_commit": None,
        "baseline_build": args.baseline_build,
        "candidate_build": args.candidate_build,
        "tree_state": git_tree_state(args.repo, ignore_dirs=(root_dir,)),
    }

    # The candidate may be a binary built from a different commit, in which case
    # its commit is recorded explicitly. Otherwise both are the current tree.
    cfg["candidate_commit"] = args.candidate_commit or cfg["baseline_commit"]
    if not args.baseline_build or not args.candidate_build:
        print("note: build descriptions are unset; provenance will not be "
              "reproducible. Pass --baseline-build and --candidate-build.",
              file=sys.stderr)

    schedule = build_schedule(openings, args.games)

    print(f"experiment : {args.experiment_id}")
    print(f"baseline   : {args.baseline_name}  {args.baseline}")
    print(f"candidate  : {args.candidate_name}  {args.candidate}")
    print(f"suite      : {args.suite} ({len(openings)} openings)")
    print(f"control    : {control_desc}   hash {args.hash_mb}MB   threads 1")
    print(f"games      : {args.games}   max plies {args.max_plies}")
    print(f"output     : {out_dir}")
    print("")

    records = []
    sprt = sprt_mod.Sprt(args.elo0, args.elo1, args.alpha, args.beta)
    started = time.time()
    run_started_utc = datetime.datetime.now(datetime.timezone.utc).isoformat()

    for opening, baseline_white, index in schedule:
        game_id = index + 1
        rec = run_one_game(cfg, opening, baseline_white, game_id, pgn_dir,
                           args.verbose)
        records.append(rec)

        if rec["is_failure"]:
            print(f"  game {game_id:3d} {rec['opening_id']:<11} "
                  f"FAILURE {rec['termination']}: {rec['error']}")
            # An infrastructure failure invalidates the run rather than being
            # scored as a loss. This is the whole point of Part 11.
            print("  stopping: an infrastructure failure is not a chess result")
            break

        sprt.add(rec["candidate_score"])
        marker = ""
        if sprt.decided:
            marker = "  << SPRT: " + sprt.decided
        print(f"  game {game_id:3d} {rec['opening_id']:<11} "
              f"{rec['result']:<8} {rec['termination']:<20} "
              f"plies={rec['plies']:3d} {rec['elapsed_s']:6.1f}s"
              f"  W/D/L {sprt.wins}/{sprt.draws}/{sprt.losses}{marker}")

        if sprt.decided:
            print("  stopping: SPRT reached a boundary")
            break

    # -- summary ----------------------------------------------------------
    finished = [r for r in records if r["is_chess_result"]]
    failures = [r for r in records if r["is_failure"]]

    colour_balance = {}
    for rec in finished:
        key = "baseline_white" if rec["baseline_white"] else "candidate_white"
        colour_balance[key] = colour_balance.get(key, 0) + 1

    summary = {
        "experiment_id": args.experiment_id,
        "started_utc": run_started_utc,
        "finished_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "elapsed_s": round(time.time() - started, 1),
        "configuration": {
            "suite": args.suite,
            "openings": len(openings),
            "games_requested": args.games,
            "control": control_desc,
            "hash_mb": args.hash_mb,
            "threads": 1,
            "options": options,
            "max_plies": args.max_plies,
            "hard_timeout_per_move_s": args.hard_timeout_per_move,
        },
        "provenance": {
            "repo": os.path.abspath(args.repo),
            "baseline_commit": cfg["baseline_commit"],
            "candidate_commit": cfg["candidate_commit"],
            "baseline_binary": engine_fingerprint(args.baseline),
            "candidate_binary": engine_fingerprint(args.candidate),
            "baseline_build": cfg["baseline_build"],
            "candidate_build": cfg["candidate_build"],
            "tree_state": cfg["tree_state"],
            "runner": "match/runner.py first-party",
            "runner_version": "1.0.0",
            "machine": {
                "platform": platform.platform(),
                "processor": platform.processor(),
                "python": platform.python_version(),
            },
            "chess_library": {
                "name": "python-chess",
                "version": chess.__version__,
            },
            "sprt": sprt_mod.__name__,
        },
        "games": records,
        "summary": {
            "chess_results": len(finished),
            "infrastructure_failures": len(failures),
            "failures_by_termination": {
                t: sum(1 for r in failures if r["termination"] == t)
                for t in sorted({r["termination"] for r in failures})
            },
            "colour_balance": colour_balance,
            "candidate_wins": sprt.wins,
            "draws": sprt.draws,
            "baseline_wins": sprt.losses,
            "total_games": sprt.games,
            "score": sprt.score(),
            "elo_estimate": sprt.elo_estimate(),
            "sprt": sprt.report(),
            "terminations": {
                t: sum(1 for r in finished if r["termination"] == t)
                for t in sorted({r["termination"] for r in finished})
            },
        },
    }

    by_opening = {}
    for rec in finished:
        bucket = by_opening.setdefault(rec["opening_id"], {"w": 0, "d": 0, "l": 0})
        score = rec["candidate_score"]
        bucket["w" if score == 1.0 else ("l" if score == 0.0 else "d")] += 1
    summary["summary"]["by_opening"] = by_opening

    with open(os.path.join(out_dir, "results.json"), "w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2)

    s = summary["summary"]
    print("")
    print(f"  chess results        : {s['chess_results']}")
    print(f"  infrastructure fails : {s['infrastructure_failures']} {s['failures_by_termination']}")
    print(f"  colour balance       : {s['colour_balance']}")
    print(f"  candidate wins       : {s['candidate_wins']}")
    print(f"  draws                : {s['draws']}")
    print(f"  baseline wins        : {s['baseline_wins']}")
    print(f"  total games          : {s['total_games']}")
    print(f"  score                : {s['score']:.4f}")
    print(f"  elo estimate         : {s['elo_estimate']:.2f}")
    print(f"  SPRT                 : {s['sprt']['decision']}  ({s['sprt']['status']})")
    print(f"  results written to   : {os.path.join(out_dir, 'results.json')}")

    # A run with an infrastructure failure is not a valid experiment. The exit
    # code says so, so a caller cannot mistake it for a completed one.
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
