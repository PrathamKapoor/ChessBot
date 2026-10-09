"""Deterministic subset extraction from a v3-format dataset.

Whole games are selected so that a subset keeps the property the trainer needs:
a game-level split inside the subset is still a game-level split. The selection
is a pure function of (dataset checksum, record count, seed), so the same
subset can be reproduced later and compared across teacher depths.

    py -3 nnue/make_subset.py --data nnue/data/v3_pool.json \\
        --records 60000 --seed 42 --out nnue/data/v3_sub60k.json
"""

import argparse
import hashlib
import json
import os
import random
from collections import defaultdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--records", type=int, required=True)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    with open(args.data, "r", encoding="utf-8") as f:
        raw = json.load(f)
    records = raw["records"]

    by_game = defaultdict(list)
    for record in records:
        by_game[record.get("game_id", "unknown")].append(record)

    games = sorted(by_game)
    random.Random(args.seed + 7717).shuffle(games)

    chosen = []
    for game in games:
        if len(chosen) >= args.records:
            break
        chosen.extend(by_game[game])
    chosen = chosen[:args.records]

    checksum = hashlib.sha256(
        json.dumps(chosen, sort_keys=True).encode()).hexdigest()[:16]
    metadata = dict(raw["metadata"])
    metadata.update({
        "subset_of": os.path.basename(args.data),
        "subset_of_checksum": raw["metadata"].get("checksum"),
        "subset_records": len(chosen),
        "subset_games": len({r.get("game_id") for r in chosen}),
        "subset_seed": args.seed,
        "subset_checksum": checksum,
    })

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump({"metadata": metadata, "records": chosen}, f)
    print(f"wrote {len(chosen)} records from "
          f"{metadata['subset_games']} games to {args.out}")
    print(f"subset_checksum {checksum}")


if __name__ == "__main__":
    main()