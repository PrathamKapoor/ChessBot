"""Does the bounded target order the mate tail better than the clip target?

The clip target collapses every |score_cp| >= 2000 label onto +/-2000, so inside
the tail the target has no ordering and a net trained on it cannot learn one.
The tanh target keeps the ordering (strictly monotone) at the cost of a
compressed output scale. That is an offline property, and this tool measures it
directly on a held-out split: rank correlation and sign agreement between the
net's prediction and the raw label, restricted to the extreme records.

    py -3 nnue/tail_analysis.py --net nnue/checkpoints/v2ctrl.pt \
        --data nnue/data/v2.json --split-mode record --seed 42
    py -3 nnue/tail_analysis.py --net nnue/checkpoints/v2tanh.pt ...

Spearman is computed in the standard way (Pearson on ranks); ties in the
predictions (a collapsed net predicting exactly +/-2000 for a whole block) get
average ranks, which is what makes the metric honest here: a net that outputs
one constant for the entire tail has zero rank correlation by construction.
"""

import argparse
import json
import math
import os
import sys

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eval_labels import load_net, to_cp  # noqa: E402
from train import load_records, split_records  # noqa: E402
from features import active_features  # noqa: E402

import chess  # noqa: E402


def average_ranks(values):
    order = sorted(range(len(values)), key=lambda i: values[i])
    ranks = [0.0] * len(values)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
            j += 1
        avg = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            ranks[order[k]] = avg
        i = j + 1
    return ranks


def pearson(xs, ys):
    n = len(xs)
    if n < 3:
        return float("nan")
    mx = sum(xs) / n
    my = sum(ys) / n
    cov = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    vx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    vy = math.sqrt(sum((y - my) ** 2 for y in ys))
    return cov / (vx * vy) if vx > 0 and vy > 0 else float("nan")


def spearman(xs, ys):
    return pearson(average_ranks(xs), average_ranks(ys))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--net", required=True)
    ap.add_argument("--data", required=True)
    ap.add_argument("--split-data", default=None)
    ap.add_argument("--split-mode", choices=["game", "record"], default="record")
    ap.add_argument("--split", type=float, default=0.8)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--batch-size", type=int, default=256)
    args = ap.parse_args()

    device = torch.device("cpu")
    model, checkpoint = load_net(args.net, device)
    target_mode = checkpoint.get("target_mode", "clip")
    tanh_scale = checkpoint.get("args", {}).get("tanh_scale", 2000.0)

    _, records = load_records(args.data)
    if args.split_data:
        _, split_all = load_records(args.split_data)
        _, valid = split_records(split_all, args.split_mode, args.split, args.seed)
        fens = {r["fen"] for r in valid}
        records = [r for r in records if r["fen"] in fens]

    preds, labels = [], []
    for start in range(0, len(records), args.batch_size):
        chunk = records[start:start + args.batch_size]
        feats = [active_features(chess.Board(r["fen"])) for r in chunk]
        with torch.no_grad():
            outs = model(feats).cpu().tolist()
        for record, out in zip(chunk, outs):
            preds.append(to_cp(out, target_mode, tanh_scale))
            labels.append(record["score_cp"])

    print(f"net {os.path.basename(args.net)}  target_mode={target_mode}  "
          f"records={len(records)}")
    for threshold in (1000.0, 2000.0, 10000.0):
        idx = [i for i, l in enumerate(labels) if abs(l) >= threshold]
        if len(idx) < 10:
            print(f"  |label| >= {threshold:7.0f}: n={len(idx)} (too few)")
            continue
        p = [preds[i] for i in idx]
        l = [labels[i] for i in idx]
        same_sign = sum(1 for x, y in zip(p, l) if (x > 0) == (y > 0))
        mae = sum(abs(x - y) for x, y in zip(p, l)) / len(idx)
        spread = max(p) - min(p)
        print(f"  |label| >= {threshold:7.0f}: n={len(idx):6d}  "
              f"spearman={spearman(p, l):+.3f}  "
              f"sign_agree={100.0*same_sign/len(idx):5.1f}%  "
              f"mae={mae:7.1f}cp  pred_spread={spread:7.1f}cp  "
              f"pred[sd]={math.sqrt(sum((x-sum(p)/len(p))**2 for x in p)/len(p)):6.1f}cp")
    return 0


if __name__ == "__main__":
    sys.exit(main())
