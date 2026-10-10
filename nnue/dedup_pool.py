"""Remove duplicate positions from an assembled pool, in place.

The clean pool was assembled from chunk shards written by two runs of the
generator, and the pre-resume chunks carried 1,289 exact duplicate positions
because of a bug in the resume patch (see generate_v3.py's comment: the deduped
list was overwritten with the undeduped harvest before chunking). The dataset
gate caught it on the finished pool - one duplicate in the 20k sample - and
this tool repairs the file without touching any label.

Dedupe rule: the position without move counters (placement + turn + castling +
en passant), the same key generate_v3.py uses. Two records matching on it have
identical HalfKP features and identical teacher labels, so keeping both would
double-weight the sample; the first occurrence wins, order is preserved, and
the counts are reported.

    py -3 nnue/dedup_pool.py --data nnue/data/v4_pool.json --in-place
"""

import argparse
import hashlib
import json
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", default=None)
    ap.add_argument("--in-place", action="store_true")
    args = ap.parse_args()

    out_path = args.data if args.in_place else args.out
    if not out_path:
        raise SystemExit("pass --out or --in-place")

    with open(args.data, "r", encoding="utf-8") as f:
        raw = json.load(f)
    records = raw["records"]

    seen = set()
    out = []
    for record in records:
        key = " ".join(record["fen"].split(" ")[:4])
        if key in seen:
            continue
        seen.add(key)
        out.append(record)

    removed = len(records) - len(out)
    checksum = hashlib.sha256(
        json.dumps(out, sort_keys=True).encode()).hexdigest()[:16]
    metadata = dict(raw["metadata"])
    metadata["positions"] = len(out)
    metadata["dedupe_removed"] = removed
    metadata["dedupe_rule"] = "fen without move counters (generate_v3.py rule)"
    metadata["checksum"] = checksum

    tmp = out_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump({"metadata": metadata, "records": out}, f, indent=2)
    os.replace(tmp, out_path)

    print(f"{args.data}: {len(records)} -> {len(out)} records "
          f"({removed} duplicates removed), checksum {checksum}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
