"""Aggregate every training curve into one table.

Phase 7 requires the learning curve (dataset size -> validation) and compute
accounting (strength per unit of compute) to rest on recorded numbers, not on
recalled ones. Each training run already writes a curve JSON with full history,
bucket metrics and compute; this script is the single place those are read.

    py -3 nnue/curve_report.py                     # print the table
    py -3 nnue/curve_report.py --md                # markdown rows for RESEARCH.md
    py -3 nnue/curve_report.py --out reports.json  # full aggregate
"""

import argparse
import glob
import json
import os
import sys


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def row(curve):
    buckets = curve.get("buckets_best", {})
    compute = curve.get("compute", {})
    return {
        "tag": curve.get("tag") or os.path.basename(curve.get("checkpoint", "?")),
        "curve": None,
        "dataset": os.path.basename(curve.get("dataset", "?")),
        "checksum": curve.get("dataset_checksum"),
        "train_records": curve.get("train_records"),
        "valid_records": curve.get("valid_records"),
        "split_mode": curve.get("split_mode"),
        "sampling": (curve.get("sampling") or {}).get("policy"),
        "sampling_boost": (curve.get("sampling") or {}).get("boost"),
        "target": curve.get("target_mode"),
        "loss": curve.get("loss_mode"),
        "lr": curve.get("lr"),
        "epochs": curve.get("epochs"),
        "best_epoch": curve.get("best_epoch"),
        "best_val_loss": round(curve["best_val_loss"], 4),
        "best_val_mae_cp": round(curve["best_val_mae_cp"], 1),
        "final_val_mae_cp": round(curve["final_val_mae_cp"], 1),
        "mae_tactical": round(buckets.get("tactical", {}).get("mae_cp", -1), 1),
        "mae_endgame": round(buckets.get("endgame", {}).get("mae_cp", -1), 1),
        "mae_quiet": round(buckets.get("quiet", {}).get("mae_cp", -1), 1),
        "train_seconds": compute.get("total_seconds"),
        "examples_per_second": compute.get("examples_per_second"),
        "gpu": compute.get("gpu"),
        "torch": (compute.get("environment") or {}).get("torch"),
        "checkpoint": curve.get("checkpoint"),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--curves", default="nnue/curves")
    ap.add_argument("--out", default=None)
    ap.add_argument("--md", action="store_true")
    args = ap.parse_args()

    rows = []
    for path in sorted(glob.glob(os.path.join(args.curves, "*.json"))):
        curve = load(path)
        if not isinstance(curve, dict) or "history" not in curve:
            continue  # e.g. holdout files
        entry = row(curve)
        entry["curve"] = path.replace("\\", "/")
        rows.append(entry)

    rows.sort(key=lambda r: (r["dataset"], r["train_records"] or 0))

    if args.md:
        print("| tag | dataset | train | target | loss | best ep | val MAE cp | "
              "tactical | endgame | quiet | minutes |")
        print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
        for r in rows:
            minutes = (r["train_seconds"] or 0) / 60
            print(f"| {r['tag']} | {r['dataset']} | {r['train_records']} | "
                  f"{r['target']} | {r['loss']} | {r['best_epoch']} | "
                  f"{r['best_val_mae_cp']} | {r['mae_tactical']} | "
                  f"{r['mae_endgame']} | {r['mae_quiet']} | {minutes:.0f} |")
    else:
        print(f"{'tag':22s} {'dataset':16s} {'train':>7s} {'tgt':>5s} {'loss':>6s} "
              f"{'bep':>4s} {'valMAE':>7s} {'tact':>6s} {'endg':>6s} {'quiet':>6s} "
              f"{'min':>5s}")
        for r in rows:
            minutes = (r["train_seconds"] or 0) / 60
            print(f"{r['tag']:22s} {r['dataset']:16s} {r['train_records']:7d} "
                  f"{r['target']:>5s} {r['loss']:>6s} {r['best_epoch']:4d} "
                  f"{r['best_val_mae_cp']:7.1f} {r['mae_tactical']:6.1f} "
                  f"{r['mae_endgame']:6.1f} {r['mae_quiet']:6.1f} {minutes:5.1f}")

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump({"runs": rows}, f, indent=2)
        print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
