"""Measure teacher (frozen classical engine) labelling throughput by depth.

The Phase 7 plan needs a teacher-depth ladder, and the ladder is only worth
running if the deeper rungs are affordable. This measures nodes/second per worker
and the resulting wall-clock rate for a given worker count, so the experiment
sizes are chosen from measurement instead of optimism.

    py -3 -u nnue/teacher_rate.py --depths 7,9,11 --positions 200 --workers 14
"""

import argparse
import json
import os
import time
from concurrent.futures import ThreadPoolExecutor

import chess
import chess.engine

ENGINE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "build", "pocketchess.exe")


def load_fens(path, count):
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    return [r["fen"] for r in raw["records"][:count]]


def label_chunk(fens, depth, workers):
    engine = chess.engine.SimpleEngine.popen_uci(ENGINE)
    out = []
    try:
        for fen in fens:
            try:
                engine.protocol.send_line("ucinewgame")
                result = engine.play(chess.Board(fen),
                                      chess.engine.Limit(depth=depth),
                                      info=chess.engine.INFO_ALL)
            except Exception:
                break
            score = result.info.get("score") if result.info else None
            rel = score.relative if score else None
            if rel is None:
                continue
            cp = (30000.0 if rel.mate() > 0 else -30000.0) if rel.is_mate() \
                else float(rel.score())
            out.append({"fen": fen, "depth": depth, "cp": cp,
                        "nodes": result.info.get("nodes", 0) if result.info else 0})
    finally:
        try:
            engine.close()
        except Exception:
            pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="nnue/data/v3_pool.json")
    ap.add_argument("--depths", default="7,9,11")
    ap.add_argument("--positions", type=int, default=200)
    ap.add_argument("--workers", type=int, default=14)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    fens = load_fens(args.data, args.positions * 8)
    report = {"engine": ENGINE, "workers": args.workers,
              "cpu_threads_available": os.cpu_count(), "depths": {}}

    for depth in [int(d) for d in args.depths.split(",")]:
        subset = fens[:args.positions]
        chunk = max(1, len(subset) // args.workers + 1)
        chunks = [subset[i:i + chunk] for i in range(0, len(subset), chunk)]
        t0 = time.time()
        results = []
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            for out in pool.map(lambda c: label_chunk(c, depth, args.workers), chunks):
                results.extend(out)
        elapsed = time.time() - t0
        rate = len(results) / max(elapsed, 1e-9)
        nodes = sum(r["nodes"] for r in results) / max(len(results), 1)
        entry = {
            "seconds": round(elapsed, 1),
            "labelled": len(results),
            "positions_per_second": round(rate, 2),
            "mean_nodes_per_position": round(nodes, 0),
            "hours_for_20k": round(20000 / rate / 3600, 2),
            "hours_for_60k": round(60000 / rate / 3600, 2),
            "hours_for_250k": round(250000 / rate / 3600, 2),
        }
        report["depths"][str(depth)] = entry
        print(f"depth {depth:2d}  {len(results):4d} positions in {elapsed:6.1f}s  "
              f"{rate:6.2f}/s  {nodes:9.0f} nodes/pos  "
              f"20k={entry['hours_for_20k']:.2f}h  "
              f"60k={entry['hours_for_60k']:.2f}h  "
              f"250k={entry['hours_for_250k']:.2f}h", flush=True)

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
        print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
