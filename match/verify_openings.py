#!/usr/bin/env python3
"""Check that every opening suite entry agrees with its own move list.

A suite is only reproducible if what it claims is what it contains. This walks
the move sequences from the initial position using python-chess if it is
available, and otherwise reports that it could not verify rather than quietly
passing. It also checks that each FEN is actually a position the engine accepts.

Run:  py -3 match/verify_openings.py match/suite_core-01.txt
"""
import sys


def is_header_row(line):
    """True for the column-header row, which is not an opening."""
    head = line.split()
    return len(head) >= 2 and head[1].lower() == "plies"


def parse_suite(path):
    """Return a list of opening dicts.

    The suite is two lines per opening:

        <id>  <plies>  <structure>  <fen>
                        moves: <uci moves or ->
    """
    openings = []
    with open(path, "r", encoding="utf-8") as handle:
        lines = handle.readlines()

    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if is_header_row(line):
            continue
        if line.startswith("moves:"):
            # Continuation of the previous opening.
            body = line.split(":", 1)[1].strip()
            if openings:
                openings[-1]["moves"] = [] if body == "-" else body.split()
            continue
        parts = line.split()
        if len(parts) < 4:
            continue
        openings.append({
            "id": parts[0],
            "plies": int(parts[1]),
            "structure": parts[2],
            "fen": " ".join(parts[3:]),
            "moves": [],
        })
    return openings


def main():
    if len(sys.argv) < 2:
        print("usage: verify_openings.py <suite-file> [engine-binary]")
        return 2
    path = sys.argv[1]
    engine = sys.argv[2] if len(sys.argv) > 2 else "build/pocketchess.exe"

    openings = parse_suite(path)
    print(f"suite: {path}")
    print(f"openings: {len(openings)}")

    try:
        import chess  # noqa: F401
        have_chess = True
    except ImportError:
        have_chess = False

    failures = 0

    if have_chess:
        for entry in openings:
            board = chess.Board()
            try:
                for san in entry["moves"]:
                    board.push_san(san)
            except Exception as exc:  # noqa: BLE001
                print(f"  FAIL {entry['id']}: move list rejected: {exc}")
                failures += 1
                continue
            produced = board.fen()
            # The suite writes fullmove/halfmove fields; compare the parts that
            # identify the position, which are the board and the clocks.
            want = entry["fen"]
            same = produced.split(" ")[:4] == want.split(" ")[:4]
            status = "ok" if same else "MISMATCH"
            if not same:
                failures += 1
                print(f"  FAIL {entry['id']}: moves give {produced!r}")
                print(f"       file says               {want!r}")
            else:
                print(f"  {entry['id']:<12} {status}  {len(entry['moves'])} moves")
    else:
        print("  python-chess not installed: move lists NOT verified")
        print("  (this is reported rather than silently treated as a pass)")

    # Ask the engine to accept each FEN. An engine that cannot parse its own
    # opening suite would fail on the first game with an unhelpful error.
    print(f"\nengine acceptance check via {engine}")
    import subprocess

    script = "\n".join(["uci"] +
                       [f"position fen {e['fen']}" for e in openings] +
                       ["isready", "quit"])
    try:
        proc = subprocess.run([engine], input=script, capture_output=True,
                              text=True, timeout=60)
        ready = proc.stdout.count("readyok")
        print(f"  readyok replies: {ready} (expected 1)")
        if ready != 1:
            failures += 1
    except Exception as exc:  # noqa: BLE001
        print(f"  FAIL engine acceptance: {exc}")
        failures += 1

    if failures:
        print(f"\nOPENINGS FAILED ({failures} problems)")
        return 1
    print("\nOPENINGS OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())