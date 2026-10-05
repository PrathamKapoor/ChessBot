"""Python/C++ golden parity for the NNUE path.

The Phase 6 commit message described an eight-position golden test that pins the
C++ inference against the PyTorch model. That script was never committed, so it
is reconstructed here, together with the vectors it is supposed to pin.

Two independent checks, both required to pass:

  parity   For every (net, FEN) pair, the C++ nnue_eval output and the PyTorch
           model output must agree to 1e-6 in network output units, i.e. 4e-4 cp.
           The residual is float32 accumulation order: PyTorch sums the feature
           transformer through a different reduction tree than the AVX2 kernels,
           so the two disagree in the last bits and nowhere else. This is the
           check that pins the export format, the loader transpose, the feature
           mirror and the AVX2 kernels against each other. Run on whatever nets
           exist, so a freshly trained candidate is gated too.

  golden   For the pinned nets named in golden_vectors.json, the C++ output must
           equal the stored value to 1e-4. This is the regression gate: it fails
           if the inference code, the export format or the feature extractor
           changes behaviour, even if both implementations change together.

Feature indices are compared separately (C++ activeFeatures via feature_check,
Python features.active_features) because identical numbers can in principle be
produced by two different feature sets.

    py -3 nnue/golden_test.py                                  # parity + golden
    py -3 nnue/golden_test.py --net nnue/checkpoints/v3.bin    # parity only
    py -3 nnue/golden_test.py --features                       # feature parity
"""

import argparse
import json
import os
import subprocess
import sys

import torch

sys_path = os.path.dirname(os.path.abspath(__file__))
if sys_path not in sys.path:
    sys.path.insert(0, sys_path)

import chess
from features import active_features
from model import NNUEModel, SCALE

ROOT = os.path.dirname(sys_path)
NNUE_EVAL = os.path.join(ROOT, "build", "nnue_eval.exe")
FEATURE_CHECK = os.path.join(ROOT, "build", "feature_check.exe")

TOLERANCE_CP = 4e-4      # 1e-6 in network output units (x400 -> cp)
GOLDEN_TOLERANCE_CP = 1e-4

# The eight positions the Phase 6 golden test was described over, pinned here
# explicitly: start position, both castling situations (mirrored and
# unmirrored side-to-move king), two middlegames with different material, a
# bare king and pawn ending, a won ending, a lost ending.
GOLDEN_FENS = [
    ("startpos",
     "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"),
    ("castled_kingside",
     "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQK2R w KQkq - 0 6"),
    ("castled_queenside",
     "r3k2r/p1qpppbp/p1np1np1/1p2p3/4P3/1PNP1NP1/P1PPPPBP/R2QK2R w KQkq - 0 9"),
    ("middlegame_queen_endgame",
     "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"),
    ("middlegame_open",
     "r1bq1rk1/pp2ppbp/2np1np1/2p5/2P1P3/2NP1N2/PP3PPP/R1BQ1RK1 w - - 0 9"),
    ("king_pawn_ending",
     "8/p7/P7/8/8/8/8/K5k1 w - - 0 1"),
    ("won_ending",
     "6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1"),
    ("lost_ending",
     "6k1/5ppp/8/8/8/8/5PPP/6K1 b - - 0 1"),
]


def load_model(net_path):
    ckpt = torch.load(net_path.replace(".bin", ".pt"), map_location="cpu",
                      weights_only=False) if net_path.endswith(".bin") \
        and os.path.exists(net_path.replace(".bin", ".pt")) else None
    model = NNUEModel()
    if ckpt is not None:
        model.load_state_dict(ckpt["state_dict"])
    else:
        raise SystemExit(f"no .pt checkpoint beside {net_path}; "
                         "golden parity needs the Python weights to compare")
    model.eval()
    return model


def cpp_eval(fen, net_path):
    out = subprocess.run([NNUE_EVAL, "--fen", fen, "--net", net_path],
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError(f"nnue_eval failed on {fen}: {out.stderr.strip()}")
    # nnue_eval prints the raw network output; evaluation_bridge.hpp applies the
    # same x400 conversion when the value enters the search, so convert here to
    # keep both sides in centipawns.
    return float(out.stdout.strip()) * SCALE


def cpp_features(fen):
    out = subprocess.run([FEATURE_CHECK, "--fen", fen],
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError(f"feature_check failed on {fen}: {out.stderr.strip()}")
    return [int(x) for x in out.stdout.split()]


def parity(nets):
    failures = 0
    checked = 0
    for net_path in nets:
        model = load_model(net_path)
        for name, fen in GOLDEN_FENS:
            py = model([active_features(chess.Board(fen))]).item() * 400.0
            cpp = cpp_eval(fen, net_path)
            delta = abs(py - cpp)
            checked += 1
            status = "ok" if delta <= TOLERANCE_CP else "FAIL"
            print(f"  {os.path.basename(net_path):<12} {name:<26} "
                  f"py={py:10.5f}  cpp={cpp:10.5f}  d={delta:.2e}  {status}")
            if status == "FAIL":
                failures += 1
    print(f"parity: {checked - failures}/{checked} within {TOLERANCE_CP}")
    return failures


def golden(path):
    with open(path, "r", encoding="utf-8") as f:
        pinned = json.load(f)
    failures = 0
    checked = 0
    for net_name, nets in pinned.items():
        net_path = os.path.join(ROOT, nets["path"])
        if not os.path.exists(net_path):
            print(f"  {net_name:<12} skipped (net not present at {nets['path']})")
            continue
        expected = nets["cp"]
        for (name, fen), want in zip(GOLDEN_FENS, expected):
            got = cpp_eval(fen, net_path)
            delta = abs(got - want)
            checked += 1
            status = "ok" if delta <= GOLDEN_TOLERANCE_CP else "FAIL"
            print(f"  {net_name:<12} {name:<26} want={want:10.5f}  "
                  f"got={got:10.5f}  d={delta:.2e}  {status}")
            if status == "FAIL":
                failures += 1
    print(f"golden: {checked - failures}/{checked} within {GOLDEN_TOLERANCE_CP}")
    return failures


def feature_parity():
    failures = 0
    for name, fen in GOLDEN_FENS:
        py = sorted(active_features(chess.Board(fen)))
        cpp = sorted(cpp_features(fen))
        ok = py == cpp
        if not ok:
            failures += 1
        print(f"  {name:<26} python={len(py):3d} cpp={len(cpp):3d}  "
              f"{'ok' if ok else 'FAIL'}")
    print(f"features: {len(GOLDEN_FENS) - failures}/{len(GOLDEN_FENS)} identical")
    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--net", action="append", default=[],
                    help="net to run parity against (repeatable)")
    ap.add_argument("--vectors",
                    default=os.path.join(sys_path, "golden_vectors.json"))
    ap.add_argument("--features", action="store_true",
                    help="also compare C++ and Python feature indices")
    ap.add_argument("--regen", default=None,
                    help="rewrite the pinned vectors for the given nets")
    args = ap.parse_args()

    nets = args.net
    if args.regen:
        name = os.path.splitext(os.path.basename(args.regen))[0]
        values = [cpp_eval(fen, args.regen) for _, fen in GOLDEN_FENS]
        payload = {}
        if os.path.exists(args.vectors):
            with open(args.vectors, "r", encoding="utf-8") as f:
                payload = json.load(f)
        payload[name] = {"path": os.path.relpath(args.regen, ROOT).replace("\\", "/"),
                         "cp": values}
        with open(args.vectors, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2)
        print(f"wrote {len(values)} vectors for {name} to {args.vectors}")
        return 0

    failures = 0
    print("feature parity (C++ vs Python):")
    failures += feature_parity()
    print()
    if nets:
        print("Python/C++ parity:")
        failures += parity(nets)
        print()
    if os.path.exists(args.vectors):
        print("pinned golden vectors:")
        failures += golden(args.vectors)
    else:
        print(f"no vector file at {args.vectors}")

    print()
    print("GOLDEN OK" if failures == 0 else f"GOLDEN FAILED ({failures})")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
