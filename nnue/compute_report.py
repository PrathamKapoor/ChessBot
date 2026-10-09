"""Aggregate compute accounting for the Phase 7 experiments.

The research question is ultimately strength per unit of compute, so every
experiment's cost is recorded where it happens and aggregated here:

    datasets   generation and labelling seconds from the dataset registry
               (harvest + teacher search, CPU, worker count in the metadata)
    training   wall seconds, examples/s, device from each run's curve JSON
    matches    wall seconds and game count from each match's results.json

    py -3 nnue/compute_report.py
    py -3 nnue/compute_report.py --out nnue/reports_phase7_compute.json
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
    rows = []
    for entry in load(path)["datasets"]:
        label_seconds = entry.get("label_seconds")
        rows.append({
            "id": entry["id"],
            "records": entry["records"],
            "teacher_depth": entry.get("teacher_depth"),
            "label_seconds": label_seconds,
            "label_hours": round(label_seconds / 3600, 1) if label_seconds else None,
            "positions_per_second": (round(entry["records"] / label_seconds, 2)
                                     if label_seconds else None),
            "generator_version": entry.get("generator_version"),
        })
    return rows


def trainings():
    rows = []
    for path in sorted(glob.glob("nnue/curves/*.json")):
        curve = load(path)
        if not isinstance(curve, dict) or "history" not in curve:
            continue
        compute = curve.get("compute", {})
        rows.append({
            "tag": curve.get("tag"),
            "dataset": os.path.basename(curve.get("dataset", "?")),
            "train_records": curve.get("train_records"),
            "epochs": curve.get("epochs"),
            "seconds": compute.get("total_seconds"),
            "minutes": round((compute.get("total_seconds") or 0) / 60, 1),
            "examples_per_second": compute.get("examples_per_second"),
            "device": compute.get("device"),
            "gpu": compute.get("gpu"),
        })
    return rows


def matches():
    rows = []
    for path in sorted(glob.glob("match/results/*/results.json")):
        record = load(path)
        summary = record.get("summary", {})
        rows.append({
            "id": record.get("experiment_id"),
            "games": summary.get("total_games"),
            "seconds": round(record.get("elapsed_s", 0)),
            "minutes": round(record.get("elapsed_s", 0) / 60, 1),
            "seconds_per_game": round(record.get("elapsed_s", 0) /
                                      max(summary.get("total_games", 1), 1), 1),
            "infrastructure_failures": summary.get("infrastructure_failures"),
            "score": summary.get("score"),
        })
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="nnue/reports_phase7_compute.json")
    args = ap.parse_args()

    report = {"datasets": datasets(), "trainings": trainings(),
              "matches": matches()}

    print("datasets (CPU, teacher labelling):")
    for row in report["datasets"]:
        if row["label_seconds"]:
            print(f"  {row['id']:22s} {row['records']:7d} rec  "
                  f"d{row['teacher_depth']}  {row['label_hours']:5.1f} h  "
                  f"{row['positions_per_second']}/s")

    print("\ntraining (GPU):")
    total_minutes = 0.0
    for row in report["trainings"]:
        total_minutes += row["minutes"]
        print(f"  {row['tag']:22s} {row['dataset']:18s} {row['train_records']:7d} rec  "
              f"{row['epochs']:2d} ep  {row['minutes']:6.1f} min  "
              f"{row['examples_per_second']}/s")
    print(f"  total training: {total_minutes/60:.1f} h")

    print("\nmatches (CPU, both engines):")
    for row in report["matches"]:
        print(f"  {row['id']:22s} {row['games']:3d} games  {row['minutes']:6.1f} min  "
              f"{row['seconds_per_game']:6.1f} s/game  "
              f"score {row['score']}  infra {row['infrastructure_failures']}")

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
