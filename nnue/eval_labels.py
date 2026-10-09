"""Score one net against one label set, on a controlled record set.

The teacher experiment compares a depth-7 net and a depth-9 net trained on the
same positions. Validation MAE from the two training runs is not comparable:
each is measured against its own teacher. This tool evaluates a net against
whatever labels you point it at, on whatever records you point it at, so the
comparison becomes:

    d7 net vs d7 labels     (its own training metric, for reference)
    d7 net vs d9 labels     (how close the shallower net's function is to
                             the deeper teacher's verdicts)
    d9 net vs d9 labels     (its own training metric)
    d9 net vs d7 labels     (for symmetry)

The record set is the validation split of --split-data, computed with the same
train.py splitter and seeds the trainer used, so "validation" means the same
records in every row. Labels are matched by FEN from --data.

Nets trained with the bounded tanh target are inverted back to centipawns
before the error is measured (the C++ engine reads the raw output so the
compression is real for search; this tool undoes it to make the numbers
comparable), using the target mode recorded in the checkpoint unless one is
given explicitly.

    py -3 nnue/eval_labels.py --net nnue/checkpoints/d7_20k.pt \\
        --data nnue/data/v4_sub20k_d9.json \\
        --split-data nnue/data/v4_sub20k.json \\
        --out nnue/reports_phase7_labels_d7net_vs_d9.json
"""

import argparse
import json
import math
import os
import sys

import torch

sys_path = os.path.dirname(os.path.abspath(__file__))
if sys_path not in sys.path:
    sys.path.insert(0, sys_path)

from features import active_features  # noqa: E402
from model import NNUEModel  # noqa: E402
from train import bucket_of, load_records, split_records  # noqa: E402

import chess  # noqa: E402

SCALE = 400.0


def load_net(path, device):
    checkpoint = torch.load(path, map_location=device, weights_only=False)
    config = checkpoint.get("config", {})
    model = NNUEModel(feature_dim=config.get("feature_dim", 49152),
                      hidden=config.get("hidden", 256),
                      layers=config.get("layers", 2)).to(device)
    model.load_state_dict(checkpoint["state_dict"])
    model.eval()
    return model, checkpoint


def to_cp(output, target_mode, tanh_scale):
    if target_mode == "tanh":
        x = min(max(output * SCALE / tanh_scale, -0.9999), 0.9999)
        return tanh_scale * math.atanh(x)
    return output * SCALE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--net", required=True)
    ap.add_argument("--data", required=True, help="label source")
    ap.add_argument("--split-data", default=None,
                    help="dataset whose validation split defines the records "
                         "(default: all records of --data)")
    ap.add_argument("--split-mode", choices=["game", "record"], default="game")
    ap.add_argument("--split-legacy", action="store_true",
                    help="replicate the Phase 6 trainer's split exactly "
                         "(random.seed(seed); shuffle; last 20%% is validation) "
                         "so v1/v2 checkpoints can be scored on the split they "
                         "were actually held out from")
    ap.add_argument("--split", type=float, default=0.8)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--target", choices=["clip", "tanh"], default=None,
                    help="override the checkpoint's target mode")
    ap.add_argument("--tanh-scale", type=float, default=None)
    ap.add_argument("--label-clip", type=float, default=2000.0,
                    help="the clip radius the labels were trained with; the "
                         "clipped MAE is the number comparable with training "
                         "curves, the raw MAE is dominated by mate labels")
    ap.add_argument("--batch-size", type=int, default=256)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model, checkpoint = load_net(args.net, device)

    target_mode = args.target or checkpoint.get("target_mode", "clip")
    tanh_scale = args.tanh_scale or checkpoint.get("args", {}).get("tanh_scale", 2000.0)

    label_meta, label_records = load_records(args.data)
    labels = {r["fen"]: r for r in label_records}

    if args.split_data:
        _, split_records_all = load_records(args.split_data)
        if args.split_legacy:
            import random as _random
            shuffled = list(split_records_all)
            _random.seed(args.seed)
            _random.shuffle(shuffled)
            cut = int(len(shuffled) * args.split)
            _, valid = shuffled[:cut], shuffled[cut:]
        else:
            _, valid = split_records(split_records_all, args.split_mode,
                                     args.split, args.seed)
        records = [r for r in valid if r["fen"] in labels]
    else:
        records = label_records

    total_abs = 0.0
    total_signed = 0.0
    clipped_abs = 0.0
    clipped_signed = 0.0
    nomate_abs = 0.0
    nomate_n = 0
    buckets = {}
    for start in range(0, len(records), args.batch_size):
        chunk = records[start:start + args.batch_size]
        feats = [active_features(chess.Board(r["fen"])) for r in chunk]
        with torch.no_grad():
            outputs = model(feats).cpu().tolist()
        for record, output in zip(chunk, outputs):
            predicted = to_cp(output, target_mode, tanh_scale)
            label = labels[record["fen"]]["score_cp"]
            clipped = max(-args.label_clip, min(args.label_clip, label))
            error = predicted - label
            clipped_error = predicted - clipped
            total_abs += abs(error)
            total_signed += error
            clipped_abs += abs(clipped_error)
            clipped_signed += clipped_error
            if abs(label) < args.label_clip:
                nomate_abs += abs(error)
                nomate_n += 1
            key = bucket_of(record)
            bucket = buckets.setdefault(key, [0.0, 0.0, 0, 0.0, 0, 0.0])
            bucket[0] += abs(error)
            bucket[1] += error
            bucket[2] += 1
            bucket[3] += abs(clipped_error)
            bucket[4] += 1
            bucket[5] += clipped_error * clipped_error

    n = max(len(records), 1)
    total_se = sum(v[5] for v in buckets.values())
    summary = {
        "net": args.net,
        "checkpoint_epoch": checkpoint.get("epoch"),
        "net_training_dataset": checkpoint.get("dataset"),
        "net_training_checksum": checkpoint.get("dataset_checksum"),
        "net_target_mode": checkpoint.get("target_mode"),
        "labels": args.data,
        "labels_checksum": label_meta.get("checksum"),
        "label_depth": label_meta.get("depth"),
        "records": len(records),
        "split_data": args.split_data,
        "split_mode": args.split_mode,
        "split_legacy": args.split_legacy,
        "split": args.split,
        "seed": args.seed,
        "target_mode_used": target_mode,
        "label_clip": args.label_clip,
        "mae_cp": round(total_abs / n, 1),
        "bias_cp": round(total_signed / n, 1),
        "mae_clipped_cp": round(clipped_abs / n, 1),
        "bias_clipped_cp": round(clipped_signed / n, 1),
        "mae_below_clip_cp": round(nomate_abs / max(nomate_n, 1), 1),
        "records_below_clip": nomate_n,
        "buckets": {k: {"mae_cp": round(v[0] / max(v[2], 1), 1),
                        "bias_cp": round(v[1] / max(v[2], 1), 1),
                        "mae_clipped_cp": round(v[3] / max(v[4], 1), 1),
                        "clipped_squared_share": round(v[5] / max(total_se, 1e-9), 4),
                        "count": v[2]}
                    for k, v in sorted(buckets.items())},
    }
    print(f"{os.path.basename(args.net)} vs {os.path.basename(args.data)} "
          f"(depth {summary['label_depth']}): n={len(records)} "
          f"MAE_raw={summary['mae_cp']}cp MAE_clip={summary['mae_clipped_cp']}cp "
          f"MAE<clip={summary['mae_below_clip_cp']}cp")
    for k, v in summary["buckets"].items():
        print(f"  {k:10s} n={v['count']:6d} mae_raw={v['mae_cp']:8.1f}cp "
              f"mae_clip={v['mae_clipped_cp']:8.1f}cp bias={v['bias_cp']:8.1f}cp")

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)
        print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
