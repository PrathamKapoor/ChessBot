"""Relabel an existing dataset at one or more teacher depths.

Positions, game identity and category metadata are preserved exactly; only the
teacher label changes. That is what makes a depth comparison a controlled
experiment: same positions, same split, same trainer, different teacher.

Two properties the first version of this script lacked, both of which cost a
whole run when it was killed mid-flight:

  shards    Each chunk writes its own shard file as soon as it finishes. A
            killed run keeps everything already labelled; --resume skips
            shards that exist. The single-file write at the end meant a 5 hour
            run that died at 90% produced nothing at all. Chunk boundaries
            depend on the record count and the worker count, so a resume must
            repeat the same --workers and --limit as the interrupted run; the
            shards are removed on successful completion so no later run can
            inherit them by accident.
  depths    Several depths are searched in the same engine session, so the
            agreement ladder (d5 vs d7 vs d8 vs d9 vs d10) comes out of one
            pass over the positions instead of one pass per rung.

The teacher is the frozen classical engine. `ucinewgame` is sent before every
position so a label depends only on the position and the depth.

    py -3 -u nnue/relabel.py --data nnue/data/v3_sub20k.json --depth 8 \\
        --out nnue/data/v3_sub20k_d8.json --workers 14 --keep-original

Depth cost on this machine (measured by nnue/teacher_rate.py, 14 workers):

    depth  7   15.5 pos/s     depth  9    0.61 pos/s
    depth  8    2.75 pos/s    depth 10    ~0.15 pos/s
    depth 11   ~0.04 pos/s

so d8 is affordable at 20k positions (2 h), d9 only in the low tens of
thousands, and d10/d11 are sample sizes, not dataset sizes.
"""

import argparse
import glob
import hashlib
import json
import os
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import chess
import chess.engine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(ROOT, "build", "pocketchess.exe")


def git_commit():
    try:
        return subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True,
                              text=True, cwd=ROOT).stdout.strip()
    except Exception:
        return None


def label_position(board, depths, teacher):
    values = {}
    for depth in depths:
        try:
            teacher.protocol.send_line("ucinewgame")
            result = teacher.play(board, chess.engine.Limit(depth=depth),
                                  info=chess.engine.INFO_ALL)
        except Exception:
            return None
        score = result.info.get("score") if result.info else None
        if score is None:
            return None
        relative = score.relative
        if relative is None:
            continue
        values[depth] = (30000.0 if relative.mate() > 0 else -30000.0) \
            if relative.is_mate() else float(relative.score())
    return values


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--depth", type=int, required=True,
                    help="teacher depth for the new score_cp")
    ap.add_argument("--also-depth", default="",
                    help="comma-separated extra depths recorded in depth_labels "
                         "only, not promoted to score_cp")
    ap.add_argument("--workers", type=int, default=10)
    ap.add_argument("--keep-original", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--resume", action="store_true")
    args = ap.parse_args()

    extra = [int(d) for d in args.also_depth.split(",") if d.strip()]
    depths = sorted({args.depth, *extra})

    with open(args.data, "r", encoding="utf-8") as f:
        raw = json.load(f)
    records = raw["records"]
    if args.limit:
        records = records[:args.limit]
    print(f"relabelling {len(records)} records to depth {args.depth} "
          f"(also {extra}) on {args.workers} workers", flush=True)

    workers = max(1, args.workers)
    chunk_size = max(1, len(records) // (workers * 8) + 1)
    chunks = [records[i:i + chunk_size] for i in range(0, len(records), chunk_size)]

    shard_dir = args.out + ".shards"
    os.makedirs(shard_dir, exist_ok=True)

    def job(item):
        index, chunk = item
        path = os.path.join(shard_dir, f"shard_{index:05d}.json")
        if args.resume and os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                return index, json.load(f)
        teacher = chess.engine.SimpleEngine.popen_uci(ENGINE)
        out = []
        try:
            for record in chunk:
                values = label_position(chess.Board(record["fen"]), depths, teacher)
                if values is None or args.depth not in values:
                    continue
                new = dict(record)
                if args.keep_original:
                    new["original_score_cp"] = record["score_cp"]
                    new["original_depth"] = record.get("depth")
                new["score_cp"] = values[args.depth]
                new["depth"] = args.depth
                if len(depths) > 1:
                    new["depth_labels"] = {str(d): values[d] for d in depths
                                           if d in values}
                out.append(new)
        finally:
            try:
                teacher.close()
            except Exception:
                pass
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(out, f)
        os.replace(tmp, path)
        return index, out

    t0 = time.time()
    by_index = {}
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futs = [pool.submit(job, (i, c)) for i, c in enumerate(chunks)]
        done = 0
        for fut in as_completed(futs):
            index, out = fut.result()
            by_index[index] = out
            done += 1
            have = sum(len(v) for v in by_index.values())
            rate = have / max(time.time() - t0, 1e-9)
            eta = (len(records) - have) / max(rate, 1e-9)
            print(f"  chunk {done}/{len(chunks)}: {have}/{len(records)} "
                  f"({rate:.2f}/s, eta {eta/60:.1f} min)", flush=True)

    out_records = [r for i in sorted(by_index) for r in by_index[i]]
    checksum = hashlib.sha256(
        json.dumps(out_records, sort_keys=True).encode()).hexdigest()[:16]
    metadata = dict(raw["metadata"])
    metadata.update({
        "relabelled_from": os.path.basename(args.data),
        "relabelled_from_checksum": raw["metadata"].get("checksum"),
        "depth": args.depth,
        "also_depth": extra,
        "relabel_workers": workers,
        "relabel_commit": git_commit(),
        "relabel_seconds": round(time.time() - t0, 1),
        "positions": len(out_records),
        "checksum": checksum,
    })
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump({"metadata": metadata, "records": out_records}, f)

    for path in glob.glob(os.path.join(shard_dir, "*.json")):
        os.remove(path)
    try:
        os.rmdir(shard_dir)
    except OSError:
        pass

    print(f"wrote {len(out_records)} records to {args.out}", flush=True)
    print(f"checksum {checksum}", flush=True)
    print(f"total {time.time()-t0:.0f}s", flush=True)


if __name__ == "__main__":
    main()
