"""Consolidate every Phase 7 result into one summary.

The final report must not contain a recalled number; this reads the committed
artefacts and prints the tables the report is built from: datasets, training
runs, suite results, matches, and the experiment decisions' raw material.

    py -3 nnue/phase7_summary.py
    py -3 nnue/phase7_summary.py --out nnue/reports_phase7_summary.json
"""

import argparse
import glob
import json
import os
import sys


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def datasets():
    path = "nnue/reports_phase7_dataset_registry.json"
    if not os.path.exists(path):
        return []
    return load(path)["datasets"]


def training_runs():
    rows = []
    for path in sorted(glob.glob("nnue/curves/*.json")):
        curve = load(path)
        if not isinstance(curve, dict) or "history" not in curve:
            continue
        buckets = curve.get("buckets_best", {})
        rows.append({
            "tag": curve.get("tag") or os.path.basename(path),
            "dataset": os.path.basename(curve.get("dataset", "?")),
            "selected_records": curve.get("selected_records"),
            "train_records": curve.get("train_records"),
            "train_records_presampling": curve.get("train_records_presampling"),
            "valid_records": curve.get("valid_records"),
            "valid_data": os.path.basename(curve["valid_data"]) if curve.get("valid_data") else None,
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
            "buckets": {k: round(v["mae_cp"], 1) for k, v in sorted(buckets.items())},
            "minutes": round((curve.get("compute", {}).get("total_seconds") or 0) / 60, 1),
        })
    return rows


def suite_results():
    rows = []
    for path in sorted(glob.glob("nnue/reports_phase7_suite_*.json")):
        data = load(path)
        rows.append({
            "candidate": data["candidate"],
            "tactical": f"{data['tactical']['passed']}/{data['tactical']['total']}",
            "endgame": f"{data['endgame']['passed']}/{data['endgame']['total']}",
            "tactical_failures": [r["case"] for r in data["tactical"]["results"]
                                  if not r["pass"]],
            "endgame_failures": [r["case"] for r in data["endgame"]["results"]
                                 if not r["pass"]],
        })
    return rows


def matches():
    rows = []
    for path in sorted(glob.glob("match/results/*/results.json")):
        if "P7-" not in path:
            continue
        record = load(path)
        summary = record.get("summary", {})
        games = record.get("games", [])
        first = games[0] if games else {}
        rows.append({
            "id": record.get("experiment_id"),
            "candidate": first.get("candidate_engine"),
            "baseline": first.get("baseline_engine"),
            "games": summary.get("total_games"),
            "wins": summary.get("candidate_wins"),
            "draws": summary.get("draws"),
            "losses": summary.get("baseline_wins"),
            "score": summary.get("score"),
            "elo": (round(summary["elo_estimate"], 1)
                    if summary.get("elo_estimate") is not None else None),
            "infrastructure_failures": summary.get("infrastructure_failures"),
            "minutes": round(record.get("elapsed_s", 0) / 60, 1),
        })
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="nnue/reports_phase7_summary.json")
    args = ap.parse_args()

    report = {
        "datasets": datasets(),
        "training": training_runs(),
        "suites": suite_results(),
        "matches": matches(),
    }

    print("== datasets")
    for d in report["datasets"]:
        print(f"  {d['id']:24s} {d['records']:8d} rec  d{d.get('teacher_depth')}  "
              f"checksum {d.get('records_checksum') or d.get('partition_checksum')}")

    print("\n== training runs")
    for r in report["training"]:
        print(f"  {r['tag']:20s} {r['dataset']:18s} {str(r['train_records']):>7s} rec "
              f"{r['target']:>4s}/{r['loss']:>5s} {str(r['sampling']):>9s} "
              f"best ep {r['best_epoch']:>2} valMAE {r['best_val_mae_cp']:>6} cp "
              f"buckets {r['buckets']} {r['minutes']:>6} min")

    print("\n== suites")
    for s in report["suites"]:
        print(f"  {s['candidate']:10s} tactical {s['tactical']:>5s}  endgame {s['endgame']:>5s}"
              f"  tac-fails={s['tactical_failures']}")

    print("\n== matches (P7)")
    for m in report["matches"]:
        print(f"  {m['id']:20s} {str(m['candidate']):10s} vs {str(m['baseline']):10s} "
              f"{m['wins']}/{m['draws']}/{m['losses']} score={m['score']:.3f} "
              f"elo={m['elo']} infra={m['infrastructure_failures']} {m['minutes']} min")

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
