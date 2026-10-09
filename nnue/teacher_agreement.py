"""Teacher consistency: does a deeper search actually say something different?

Part 7 of the Phase 7 plan asks whether the depth-7 teacher is stable enough to
train against, rather than assuming deeper is better. The cost ladder measured
by nnue/teacher_rate.py makes deeper rungs expensive:

    depth  7   15.5 pos/s      depth  9    0.61 pos/s
    depth  8    2.75 pos/s     depth 10    ~0.15 pos/s
                          depth 11    ~0.04 pos/s

so this runs on a few hundred positions rather than on a dataset. Every position
is searched at every requested depth in the same engine session, with
`ucinewgame` before each, so the only thing that varies is the depth.

Reported per adjacent pair:

  mean_abs_cp / median_abs_cp   typical disagreement
  sign_flip_pct                 how often the better side changes
  large_disagreement_pct        |a-b| >= 100 cp and >= 300 cp
  mate_disagreement             one depth says mate, the other does not
  tactical_disagreement_pct     disagreement on positions flagged tactical
  quiet_disagreement_pct        disagreement on quiet positions, as a contrast

The tactical/quiet split matters: teacher noise that only appears in sharp
positions is what ends up as missed recaptures in a game, while noise in quiet
positions is comparatively harmless. A depth ladder that improves the average but
not the tactical spread is not worth its cost.

    py -3 -u nnue/teacher_agreement.py --data nnue/data/v3_pool.json \\
        --positions 400 --depths 5,7,8,9,10 --deep-subset 200 --depths-deep 11 \\
        --workers 14 --out nnue/reports_phase7_teacher_agreement.json
"""

import argparse
import json
import os
import statistics
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed

import chess
import chess.engine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(ROOT, "build", "pocketchess.exe")

MATE_CP = 30000.0


def tactical_of(board, score_cp):
    """Same reproducible heuristic the dataset generator uses."""
    flags = []
    if board.is_check():
        flags.append("in_check")
    legal = list(board.legal_moves)
    if any(board.is_capture(m) for m in legal):
        flags.append("capture_available")
    if any(m.promotion for m in legal):
        flags.append("promotion_available")
    if abs(score_cp) >= 500:
        flags.append("decisive")
    return flags


def label_chunk(items, depths, deep):
    """Label each (fen, index) at every depth; `deep` depths only if asked."""
    engine = chess.engine.SimpleEngine.popen_uci(ENGINE)
    out = []
    try:
        for fen, index, wants_deep in items:
            board = chess.Board(fen)
            wanted = list(depths) + (list(deep) if wants_deep else [])
            values = {}
            for depth in wanted:
                try:
                    engine.protocol.send_line("ucinewgame")
                    result = engine.play(board, chess.engine.Limit(depth=depth),
                                          info=chess.engine.INFO_ALL)
                except Exception:
                    return out
                score = result.info.get("score") if result.info else None
                rel = score.relative if score else None
                if rel is None:
                    continue
                values[depth] = (MATE_CP if rel.mate() > 0 else -MATE_CP) \
                    if rel.is_mate() else float(rel.score())
            out.append({"index": index, "fen": fen,
                        "scores": {str(d): v for d, v in values.items()},
                        "tactical": tactical_of(board, values.get(depths[-1], 0.0))})
    finally:
        try:
            engine.close()
        except Exception:
            pass
    return out


def compare(low_scores, high_scores, indices, tactical):
    """Compare two depth score maps over `indices`, split by tactical class."""
    diffs = []
    sign_flips = 0
    big100 = big300 = 0
    mate_disagree = 0
    tactical_diffs = []
    quiet_diffs = []
    for index in indices:
        sa = low_scores.get(index)
        sb = high_scores.get(index)
        if sa is None or sb is None:
            continue
        d = abs(sa - sb)
        diffs.append(d)
        if (sa > 0) != (sb > 0) and abs(sa) > 20 and abs(sb) > 20:
            sign_flips += 1
        if d >= 100:
            big100 += 1
        if d >= 300:
            big300 += 1
        if (abs(sa) >= MATE_CP) != (abs(sb) >= MATE_CP):
            mate_disagree += 1
        (tactical_diffs if tactical.get(index) else quiet_diffs).append(d)

    n = max(len(diffs), 1)
    out = {
        "pairs": len(diffs),
        "mean_abs_cp": round(statistics.fmean(diffs), 1) if diffs else None,
        "median_abs_cp": round(statistics.median(diffs), 1) if diffs else None,
        "p90_abs_cp": round(sorted(diffs)[int(0.9 * (len(diffs) - 1))], 1) if diffs else None,
        "max_abs_cp": round(max(diffs), 1) if diffs else None,
        "sign_flip_pct": round(100.0 * sign_flips / n, 2),
        "disagree_ge_100cp_pct": round(100.0 * big100 / n, 2),
        "disagree_ge_300cp_pct": round(100.0 * big300 / n, 2),
        "mate_disagreements": mate_disagree,
        "tactical_positions": len(tactical_diffs),
        "tactical_mean_abs_cp": (round(statistics.fmean(tactical_diffs), 1)
                                 if tactical_diffs else None),
        "quiet_positions": len(quiet_diffs),
        "quiet_mean_abs_cp": (round(statistics.fmean(quiet_diffs), 1)
                              if quiet_diffs else None),
    }
    if tactical_diffs and quiet_diffs:
        out["tactical_over_quiet"] = round(
            out["tactical_mean_abs_cp"] / max(out["quiet_mean_abs_cp"], 1e-9), 2)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="nnue/data/v3_pool.json")
    ap.add_argument("--positions", type=int, default=400)
    ap.add_argument("--depths", default="5,7,8,9,10")
    ap.add_argument("--depths-deep", default="11")
    ap.add_argument("--deep-subset", type=int, default=200,
                    help="how many of the positions also get the deep depths")
    ap.add_argument("--workers", type=int, default=14)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out", default="nnue/reports_phase7_teacher_agreement.json")
    args = ap.parse_args()

    depths = [int(d) for d in args.depths.split(",") if d.strip()]
    deep = [int(d) for d in args.depths_deep.split(",")
            if d.strip() and d.strip().lower() not in ("none", "off", "")]

    with open(args.data, "r", encoding="utf-8") as f:
        raw = json.load(f)
    records = raw["records"]
    # Deterministic spread across the file rather than the first N, which are
    # all from the same harvest block.
    stride = max(1, len(records) // args.positions)
    chosen = records[::stride][:args.positions]
    items = [(r["fen"], i, i < args.deep_subset) for i, r in enumerate(chosen)]
    print(f"teacher agreement: {len(items)} positions, depths {depths}, "
          f"deep {deep} on the first {args.deep_subset}, {args.workers} workers",
          flush=True)

    chunk = max(1, len(items) // args.workers + 1)
    chunks = [items[i:i + chunk] for i in range(0, len(items), chunk)]
    results = []
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futs = [pool.submit(label_chunk, c, depths, deep) for c in chunks]
        done = 0
        for fut in as_completed(futs):
            results.extend(fut.result())
            done += 1
            print(f"  chunk {done}/{len(chunks)}: {len(results)}/{len(items)} "
                  f"({time.time()-t0:.0f}s)", flush=True)

    by_index = {}
    for rec in results:
        prev = by_index.get(rec["index"])
        by_index[rec["index"]] = {
            "scores": {**(prev["scores"] if prev else {}), **rec["scores"]},
            "tactical": rec["tactical"] if prev is None
            else (prev["tactical"] or rec["tactical"]),
        }

    ordered = sorted(set(depths) | set(deep))
    tactical = {i: bool(v["tactical"]) for i, v in by_index.items()}
    scores = {depth: {i: v["scores"].get(str(depth))
                      for i, v in by_index.items()
                      if str(depth) in v["scores"]}
              for depth in ordered}

    report = {
        "engine": ENGINE,
        "dataset": os.path.basename(args.data),
        "dataset_checksum": raw["metadata"].get("checksum"),
        "positions": len(by_index),
        "deep_subset": args.deep_subset,
        "depths": depths,
        "deep_depths": deep,
        "workers": args.workers,
        "seconds": round(time.time() - t0, 1),
        "pairs": {},
    }
    for i in range(len(ordered) - 1):
        lo, hi = ordered[i], ordered[i + 1]
        indices = sorted(set(scores[lo]) & set(scores[hi]))
        result = compare(scores[lo], scores[hi], indices, tactical)
        report["pairs"][f"d{lo}_vs_d{hi}"] = result
        print(f"d{lo} vs d{hi}: pairs={result['pairs']} "
              f"mean={result['mean_abs_cp']}cp median={result['median_abs_cp']}cp "
              f"sign_flip={result['sign_flip_pct']}% "
              f">=300cp={result['disagree_ge_300cp_pct']}% "
              f"mate_disagree={result['mate_disagreements']} "
              f"tactical/quiet={result.get('tactical_over_quiet')}", flush=True)

    report["labels"] = {
        str(i): {"scores": v["scores"], "tactical": v["tactical"]}
        for i, v in sorted(by_index.items())
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
