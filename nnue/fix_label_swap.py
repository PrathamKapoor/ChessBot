"""Swap score_cp and shallow_cp back in records written by a buggy generator.

Incident this exists for: generate_v3.py returned its two labels in the wrong
order (values[depths[-1]], values[depths[0]]) so every record's `score_cp`
held the shallow-depth search and `shallow_cp` held the deep one, while the
metadata claimed depth 7. Both values were stored, so the conversion is
lossless: swapping the two fields restores the intended dataset, and no
position needs re-searching.

Caught by check_labels.py, which re-labels a sample with a fresh teacher and
requires exact reproduction: 18 of 20 sampled records had score_cp equal to a
fresh shallow-depth search. v1/v2 are unaffected (nnue/dataset.py never stored
a shallow label).

Usage:

    py -3 nnue/fix_label_swap.py --shards nnue/data/v4_pool.json.shards
    py -3 nnue/fix_label_swap.py --data nnue/data/v4_p1.json --in-place

Shard chunks are bare record lists; dataset files carry metadata, which is
updated with a marker so a second run refuses to double-swap. Every record
must carry both fields or the tool aborts - a file where only some records
were swapped is ambiguous and must not be guessed at.
"""

import argparse
import hashlib
import glob
import json
import os
import sys


def swap_records(records):
    swapped = 0
    out = []
    for record in records:
        if "shallow_cp" not in record:
            raise SystemExit(
                f"record for {record.get('fen', '?')[:40]} has no shallow_cp; "
                "cannot tell whether it was swapped - aborting rather than "
                "guessing")
        new = dict(record)
        new["score_cp"], new["shallow_cp"] = record["shallow_cp"], record["score_cp"]
        out.append(new)
        swapped += 1
    return out, swapped


def process_shard(path):
    with open(path, "r", encoding="utf-8") as f:
        records = json.load(f)
    out, swapped = swap_records(records)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(out, f)
    os.replace(tmp, path)
    return swapped


def process_dataset(path, out_path):
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    if raw["metadata"].get("label_swap_fixed"):
        raise SystemExit(f"{path} is already marked label_swap_fixed; refusing")
    out, swapped = swap_records(raw["records"])
    metadata = dict(raw["metadata"])
    metadata["label_swap_fixed"] = True
    metadata["label_swap_note"] = (
        "score_cp and shallow_cp were swapped by generate_v3.py before the "
        "label_order fix; this file has the corrected assignment")
    metadata["positions"] = len(out)
    metadata["checksum"] = hashlib.sha256(
        json.dumps(out, sort_keys=True).encode()).hexdigest()[:16]
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump({"metadata": metadata, "records": out}, f, indent=2)
    return swapped, metadata["checksum"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shards", default=None, help="directory of chunk_*.json")
    ap.add_argument("--data", default=None, help="dataset file with metadata")
    ap.add_argument("--out", default=None, help="output for --data (or --in-place)")
    ap.add_argument("--in-place", action="store_true")
    args = ap.parse_args()

    if args.shards:
        paths = sorted(glob.glob(os.path.join(args.shards, "chunk_*.json")))
        if not paths:
            raise SystemExit(f"no chunk shards in {args.shards}")
        total = 0
        for path in paths:
            swapped = process_shard(path)
            total += swapped
            print(f"  {os.path.basename(path)}: swapped {swapped}")
        print(f"swapped {total} records across {len(paths)} shards")
        return 0

    if args.data:
        out_path = args.data if args.in_place else args.out
        if not out_path:
            raise SystemExit("pass --out or --in-place")
        swapped, checksum = process_dataset(args.data, out_path)
        print(f"swapped {swapped} records -> {out_path} checksum {checksum}")
        return 0

    raise SystemExit("pass --shards or --data")


if __name__ == "__main__":
    sys.exit(main())
