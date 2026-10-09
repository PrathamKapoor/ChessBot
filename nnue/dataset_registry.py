"""Emit an immutable registry of every dataset in nnue/data.

Phase 7 requires every dataset to be reproducible from its recorded
provenance: dataset ID, generator commit, teacher depth, sampling policy,
record count, checksum and generation configuration. Format v3 already stores
that inside each file; this script aggregates it so the record lives in git
even though the data itself does not.

    py -3 nnue/dataset_registry.py --out nnue/reports_phase7_dataset_registry.json
"""

import argparse
import glob
import hashlib
import json
import os
import sys


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()[:16]


def describe(path):
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    meta = raw.get("metadata", {})
    return {
        "id": os.path.basename(path),
        "path": path.replace("\\", "/"),
        "records": len(raw.get("records", [])),
        "records_checksum": meta.get("checksum"),
        "partition_checksum": meta.get("subset_checksum"),
        "file_sha256_16": file_sha256(path),
        "file_bytes": os.path.getsize(path),
        "format": meta.get("format"),
        "generator": meta.get("generator"),
        "generator_commit": meta.get("generator_commit"),
        "teacher_depth": meta.get("depth"),
        "shallow_depth": meta.get("shallow_depth"),
        "sampling": {
            "selfplay_games": meta.get("selfplay_games"),
            "random_games": meta.get("random_games"),
            "selfplay_depth": meta.get("selfplay_depth"),
            "harvest_every": meta.get("harvest_every"),
            "harvest_every_late": meta.get("harvest_every_late"),
            "late_from_ply": meta.get("late_from_ply"),
            "min_ply": meta.get("min_ply"),
            "max_plies": meta.get("max_plies"),
            "opening_min": meta.get("opening_min"),
            "opening_max": meta.get("opening_max"),
            "seed": meta.get("seed"),
            "workers": meta.get("workers"),
        },
        "subset_of": meta.get("subset_of"),
        "subset_of_checksum": meta.get("subset_of_checksum"),
        "subset_records": meta.get("subset_records"),
        "subset_seed": meta.get("subset_seed"),
        "colour_balance": meta.get("colour_balance"),
        "label_seconds": meta.get("label_seconds"),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", default="nnue/data")
    ap.add_argument("--out", default="nnue/reports_phase7_dataset_registry.json")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.data_dir, "*.json")))
    datasets = [describe(p) for p in paths]
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump({"datasets": datasets}, f, indent=2)
    for d in datasets:
        print(f"{d['id']:24s} {d['records']:8d} records  "
              f"checksum={d['records_checksum'] or d['partition_checksum']}  "
              f"depth={d['teacher_depth']}")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
