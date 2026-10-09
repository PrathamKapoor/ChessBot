"""Assemble a dataset from a generator's chunk shards (partial or complete).

The generator writes its single output file only at the end of labelling, but
every finished chunk is already a valid, immutable block of labelled records.
This tool turns the chunks that exist right now into a dataset, with the
provenance that makes it usable for real experiments:

    py -3 nnue/assemble_pool.py --shards nnue/data/v4_pool.json.shards \\
        --out nnue/data/v4_p1.json

What it guarantees:

  * records are assembled in chunk index order (the same order the generator
    itself uses), so the same set of chunks always produces the same checksum;
  * the metadata records that this is a partial assembly: how many chunks of
    how many, the harvest provenance from harvest.json, and the generator
    configuration from config.json (which the generator validates on resume);
  * colour balance is recomputed and printed (the v3 defect must be visible);
  * it refuses to overwrite an existing output unless --force.

A partial assembly is a uniform sample of the harvest's positions: chunks are
slices of the shuffled unique-position list, so a prefix of completed chunks
is not a prefix of games - a game contributes only the positions whose chunks
happen to have finished. That is fine for training and for a game-level split
(every record keeps its game_id, so a game never straddles the split), and it
is recorded in the metadata so the difference is never silent.
"""

import argparse
import hashlib
import json
import math
import os
import sys
from collections import Counter


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shards", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    if os.path.exists(args.out) and not args.force:
        raise SystemExit(f"{args.out} exists; pass --force to overwrite "
                         "(datasets in this phase are immutable once used)")

    chunk_paths = sorted(p for p in os.listdir(args.shards)
                         if p.startswith("chunk_") and p.endswith(".json"))
    if not chunk_paths:
        raise SystemExit(f"no chunk shards in {args.shards}")

    with open(os.path.join(args.shards, "config.json"), "r", encoding="utf-8") as f:
        config = json.load(f)
    harvest = None
    harvest_path = os.path.join(args.shards, "harvest.json")
    if os.path.exists(harvest_path):
        with open(harvest_path, "r", encoding="utf-8") as f:
            harvest = json.load(f)

    records = []
    for name in chunk_paths:
        with open(os.path.join(args.shards, name), "r", encoding="utf-8") as f:
            records.extend(json.load(f))

    unique = harvest["positions"] if harvest else len(chunk_paths)
    workers = config.get("workers", 8)
    chunk_size = max(1, len(unique) // (workers * 8) + 1)
    total_chunks = math.ceil(len(unique) / chunk_size) if harvest else len(chunk_paths)

    checksum = hashlib.sha256(
        json.dumps(records, sort_keys=True).encode()).hexdigest()[:16]

    import chess
    white = 0
    phase = Counter()
    for record in records:
        if chess.Board(record["fen"]).turn:
            white += 1
        phase[record["phase"]] += 1
    total = max(len(records), 1)
    balance = {
        "white": white,
        "black": len(records) - white,
        "white_pct": round(100.0 * white / total, 2),
    }

    metadata = {
        "format": config.get("format", 3),
        "generator_version": config.get("generator_version"),
        "generator_config": {k: v for k, v in config.items()
                             if k in ("depth", "shallow_depth", "selfplay_games",
                                      "random_games", "selfplay_depth",
                                      "harvest_every", "harvest_every_late",
                                      "late_from_ply", "min_ply", "max_plies",
                                      "seed", "workers")},
        "positions": len(records),
        "partial_assembly": len(chunk_paths) < total_chunks,
        "chunks_included": len(chunk_paths),
        "chunks_total": total_chunks,
        "harvested": harvest["harvested"] if harvest else None,
        "unique_harvested": len(harvest["positions"]) if harvest else None,
        "colour_balance": balance,
        "phase_histogram": dict(phase),
        "checksum": checksum,
        "assembled_from": os.path.basename(args.shards),
        "assembler": "nnue/assemble_pool.py",
    }

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump({"metadata": metadata, "records": records}, f, indent=2)

    print(f"{args.out}: {len(records)} records from {len(chunk_paths)}/"
          f"{total_chunks} chunks")
    print(f"  colour white {balance['white_pct']}%  phase {dict(phase)}")
    print(f"  partial={metadata['partial_assembly']}  checksum {checksum}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
