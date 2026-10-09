"""Train the NNUE model, Phase 7 controlled trainer.

The architecture is frozen (HalfKP 49,152 features, FT 49152->256, three ReLU
hidden layers, scalar output, PCNN v1 export). Everything this script can vary
is a training or data variable:

  data selection   --max-records (whole games, deterministic), --sampling
  splits           --split-mode game|record (game is the default and the
                    correct choice: positions a few plies apart inside one game
                    are near-duplicates, so a record-level split leaks)
  targets          --target clip|tanh  (see TARGETS below)
  loss             --loss huber|mse
  schedule         --lr, --lr-milestones, --lr-gamma, --weight-decay, --epochs

Every run writes a curve JSON with the full per-epoch history, compute
accounting and per-category validation metrics so that the learning curve and
the "validation is not strength" checks rest on recorded numbers.

TARGETS
  clip  y = clamp(score_cp, -C, C) / SCALE                (Phase 6 behaviour)
        Hard saturation: every mate score and every |score| >= C collapses onto
        exactly +-C/SCALE, so a large share of the data trains a single output
        value with no gradient ordering inside the tail.
  tanh  y = SCALE_TANH * tanh(score_cp / SCALE_TANH) / SCALE
        Monotone and strictly bounded, with the same range as clip, so the
        C++ inference path (output * SCALE) is untouched and only the target
        representation changes. Documented in RESEARCH.md Phase 7.

A probabilistic WDL head was considered and rejected for this phase: it would
change the mapping from raw output to centipawns and therefore the frozen
inference, which Part 1 forbids.
"""

import argparse
import json
import math
import os
import random
import sys
import time
from collections import defaultdict

import torch
import torch.nn as nn

import chess

sys_path_here = os.path.dirname(os.path.abspath(__file__))
if sys_path_here not in sys.path:
    sys.path.insert(0, sys_path_here)
from features import active_features
from model import NNUEModel, SCALE

DEFAULT_CLIP = 2000.0
DEFAULT_TANH_SCALE = 2000.0


# --------------------------------------------------------------------------
# data selection
# --------------------------------------------------------------------------

def load_records(path):
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    return raw["metadata"], raw["records"]


def select_games(records, max_records, seed):
    """Deterministically take whole games until max_records is reached."""
    if not max_records or max_records >= len(records):
        return records
    by_game = defaultdict(list)
    for record in records:
        by_game[record.get("game_id", "unknown")].append(record)
    games = sorted(by_game)
    rng = random.Random(seed + 7717)
    rng.shuffle(games)
    chosen = []
    for game in games:
        if len(chosen) >= max_records:
            break
        chosen.extend(by_game[game])
    return chosen[:max_records]


def apply_sampling_policy(records, policy, boost, seed):
    """Rebalance a selected pool under a documented sampling policy.

    baseline  every selected record is used once
    tactical  records flagged tactical are repeated `boost` times
    endgame   records labelled with an endgame class are repeated `boost` times

    Repetition rather than deletion: it keeps the pool immutable and makes the
    oversampling ratio explicit in the run record.
    """
    if policy == "baseline":
        return records, {"policy": "baseline", "boost": 1}

    out = []
    counts = defaultdict(int)
    for record in records:
        keep = 1
        if policy == "tactical":
            flags = set(record.get("flags", []))
            volatile = ("shallow_cp" in record and
                        abs(record["score_cp"] - record["shallow_cp"]) >= 200)
            if flags & {"in_check", "promotion_available"} or volatile:
                keep = boost
        elif policy == "endgame":
            labels = set(record.get("endgame", []))
            if record.get("phase") == "endgame" or labels & {
                    "pawnless", "pawn_endgame", "minor_ending", "rook_ending",
                    "queen_ending", "sparse_pieces", "opposite_bishops"}:
                keep = boost
        counts[keep] += 1
        for _ in range(keep):
            out.append(record)
    return out, {"policy": policy, "boost": boost,
                 "weight_histogram": dict(counts)}


def split_records(records, mode, split, seed):
    """Game-level or record-level train/validation split."""
    if mode == "game":
        games = sorted({r.get("game_id", "unknown") for r in records})
        rng = random.Random(seed + 4241)
        rng.shuffle(games)
        cut = int(len(games) * split)
        train_games = set(games[:cut])
        train = [r for r in records if r.get("game_id", "unknown") in train_games]
        valid = [r for r in records if r.get("game_id", "unknown") not in train_games]
    else:
        shuffled = list(records)
        random.Random(seed + 4241).shuffle(shuffled)
        cut = int(len(shuffled) * split)
        train, valid = shuffled[:cut], shuffled[cut:]
    return train, valid


# --------------------------------------------------------------------------
# targets and metrics
# --------------------------------------------------------------------------

def make_targets(records, mode, clip, tanh_scale):
    if mode == "clip":
        return torch.tensor(
            [max(-clip, min(clip, r["score_cp"])) / SCALE for r in records],
            dtype=torch.float32)
    if mode == "tanh":
        return torch.tensor(
            [tanh_scale * math.tanh(r["score_cp"] / tanh_scale) / SCALE
             for r in records], dtype=torch.float32)
    raise ValueError(f"unknown target mode {mode}")


def bucket_of(record):
    """Reproducible metric buckets, evaluated on the validation split only."""
    flags = set(record.get("flags", []))
    endgame = set(record.get("endgame", []))
    score = record["score_cp"]
    volatile = ("shallow_cp" in record and
                abs(record["score_cp"] - record["shallow_cp"]) >= 200)
    if flags & {"in_check", "promotion_available"} or volatile:
        return "tactical"
    if record.get("phase") == "endgame" or endgame:
        return "endgame"
    if abs(score) >= 1000:
        return "extreme"
    if (not flags & {"in_check", "capture_available"}) and abs(score) <= 100:
        return "quiet"
    return "ordinary"


class FeatureCache:
    """Features are a pure function of the FEN; compute each position once."""

    def __init__(self, records):
        t0 = time.time()
        self.feats = [active_features(chess.Board(r["fen"])) for r in records]
        self.seconds = time.time() - t0

    def __len__(self):
        return len(self.feats)


def collate(batch):
    feats = [f for f, _ in batch]
    targets = torch.stack([t for _, t in batch])
    return feats, targets


def evaluate(model, records, cache, targets, batches, device, loss_fn):
    model.eval()
    total_loss = 0.0
    total_abs = 0.0
    buckets = defaultdict(lambda: [0.0, 0])
    with torch.no_grad():
        for start in range(0, len(records), batches):
            chunk = records[start:start + batches]
            feats = cache.feats[start:start + batches]
            target = targets[start:start + batches].to(device)
            pred = model(feats)
            total_loss += loss_fn(pred, target).item() * len(chunk)
            errors = torch.abs(pred - target).cpu()
            total_abs += errors.sum().item()
            for offset, record in enumerate(chunk):
                key = bucket_of(record)
                buckets[key][0] += errors[offset].item()
                buckets[key][1] += 1
    n = max(len(records), 1)
    out = {
        "loss": total_loss / n,
        "mae_normalised": total_abs / n,
        "mae_cp": SCALE * total_abs / n,
        "buckets": {k: {"mae_normalised": v[0] / max(v[1], 1),
                        "mae_cp": SCALE * v[0] / max(v[1], 1),
                        "count": v[1]}
                    for k, v in sorted(buckets.items())},
    }
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--batch-size", type=int, default=64)
    ap.add_argument("--lr", type=float, default=5e-4)
    ap.add_argument("--lr-milestones", type=str, default="8,14")
    ap.add_argument("--lr-gamma", type=float, default=0.5)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--weight-decay", type=float, default=1e-4)
    ap.add_argument("--split", type=float, default=0.8)
    ap.add_argument("--split-mode", choices=["game", "record"], default="game")
    ap.add_argument("--max-records", type=int, default=0,
                    help="subsample whole games up to this many records (0 = all)")
    ap.add_argument("--sampling", choices=["baseline", "tactical", "endgame"],
                    default="baseline")
    ap.add_argument("--sampling-boost", type=int, default=3)
    ap.add_argument("--target", choices=["clip", "tanh"], default="clip")
    ap.add_argument("--clip", type=float, default=DEFAULT_CLIP)
    ap.add_argument("--tanh-scale", type=float, default=DEFAULT_TANH_SCALE)
    ap.add_argument("--loss", choices=["huber", "mse"], default="huber")
    ap.add_argument("--huber-delta", type=float, default=1.0)
    ap.add_argument("--curve-out", default=None)
    ap.add_argument("--tag", default="", help="free-form experiment label")
    ap.add_argument("--holdout", action="store_true",
                    help="export the validation split for offline evaluation")
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    random.seed(args.seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(args.seed)

    t_start = time.time()
    metadata, all_records = load_records(args.data)
    selected = select_games(all_records, args.max_records, args.seed)
    selected, sampling_info = apply_sampling_policy(
        selected, args.sampling, args.sampling_boost, args.seed)
    train_records, valid_records = split_records(
        selected, args.split_mode, args.split, args.seed)

    train_games = {r.get("game_id") for r in train_records}
    valid_games = {r.get("game_id") for r in valid_records}
    overlap = len((train_games & valid_games) - {None})
    exact_overlap = len({r["fen"] for r in train_records} &
                        {r["fen"] for r in valid_records})

    print(f"pool={len(all_records)} selected={len(selected)} "
          f"train={len(train_records)} valid={len(valid_records)} "
          f"sampling={sampling_info['policy']} boost={sampling_info['boost']}")
    print(f"split_mode={args.split_mode} game_overlap={overlap} "
          f"exact_fen_overlap={exact_overlap}")

    train_cache = FeatureCache(train_records)
    valid_cache = FeatureCache(valid_records)
    print(f"features precomputed in {train_cache.seconds + valid_cache.seconds:.1f}s")

    train_targets = make_targets(train_records, args.target, args.clip, args.tanh_scale)
    valid_targets = make_targets(valid_records, args.target, args.clip, args.tanh_scale)

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    gpu_name = (torch.cuda.get_device_name(0) if device.type == "cuda" else "cpu")
    print(f"device={device} gpu={gpu_name}")

    model = NNUEModel().to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr,
                                  weight_decay=args.weight_decay)
    milestones = [int(m) for m in args.lr_milestones.split(",") if m.strip()]
    scheduler = torch.optim.lr_scheduler.MultiStepLR(optimizer, milestones=milestones,
                                                     gamma=args.lr_gamma)
    loss_fn = nn.HuberLoss(delta=args.huber_delta) if args.loss == "huber" \
        else nn.MSELoss()

    history = []
    best_val = float("inf")
    best_epoch = -1
    steps_total = 0
    train_examples = 0
    for epoch in range(args.epochs):
        t0 = time.time()
        model.train()
        total_loss = 0.0
        order = list(range(len(train_records)))
        random.Random(args.seed * 1000 + epoch).shuffle(order)
        steps = 0
        for start in range(0, len(order), args.batch_size):
            idx = order[start:start + args.batch_size]
            feats = [train_cache.feats[i] for i in idx]
            target = train_targets[idx].to(device)
            optimizer.zero_grad()
            pred = model(feats)
            loss = loss_fn(pred, target)
            loss.backward()
            optimizer.step()
            total_loss += loss.item() * len(idx)
            steps += 1
        scheduler.step()

        metrics = evaluate(model, valid_records, valid_cache, valid_targets,
                           args.batch_size, device, loss_fn)
        elapsed = time.time() - t0
        steps_total += steps
        train_examples += len(train_records)
        lr = optimizer.param_groups[0]["lr"]

        improved = metrics["loss"] < best_val
        if improved:
            best_val = metrics["loss"]
            best_epoch = epoch + 1
            os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
            torch.save({
                "state_dict": model.state_dict(),
                "config": {"feature_dim": model.feature_dim, "hidden": model.hidden,
                           "layers": model.layers},
                "dataset": args.data,
                "dataset_checksum": metadata.get("checksum"),
                "seed": args.seed,
                "epochs": args.epochs,
                "epoch": epoch + 1,
                "val_loss": metrics["loss"],
                "val_mae_cp": metrics["mae_cp"],
                "best_val_loss": best_val,
                "target_mode": args.target,
                "loss_mode": args.loss,
                "split_mode": args.split_mode,
                "sampling": sampling_info,
                "args": vars(args),
            }, args.output)

        history.append({
            "epoch": epoch + 1,
            "train_loss": total_loss / max(len(train_records), 1),
            "val_loss": metrics["loss"],
            "val_mae_normalised": metrics["mae_normalised"],
            "val_mae_cp": metrics["mae_cp"],
            "buckets": metrics["buckets"],
            "lr": lr,
            "seconds": elapsed,
            "examples_per_second": len(train_records) / max(elapsed, 1e-9),
            "steps_per_second": steps / max(elapsed, 1e-9),
            "best": improved,
        })
        print(f"epoch {epoch+1:3d}  train_loss={history[-1]['train_loss']:.4f}  "
              f"val_loss={metrics['loss']:.4f}  val_mae={metrics['mae_cp']:.0f}cp  "
              f"lr={lr:g}  {elapsed:.0f}s  "
              f"{len(train_records)/max(elapsed,1e-9):.0f}ex/s{'  *' if improved else ''}")

    total_seconds = time.time() - t_start
    summary = {
        "tag": args.tag,
        "dataset": args.data,
        "dataset_checksum": metadata.get("checksum"),
        "dataset_records": len(all_records),
        "selected_records": len(selected),
        "train_records": len(train_records),
        "valid_records": len(valid_records),
        "sampling": sampling_info,
        "split_mode": args.split_mode,
        "split": args.split,
        "split_game_overlap": overlap,
        "split_exact_fen_overlap": exact_overlap,
        "target_mode": args.target,
        "clip": args.clip,
        "tanh_scale": args.tanh_scale,
        "loss_mode": args.loss,
        "huber_delta": args.huber_delta,
        "optimizer": "AdamW",
        "lr": args.lr,
        "lr_milestones": milestones,
        "lr_gamma": args.lr_gamma,
        "weight_decay": args.weight_decay,
        "batch_size": args.batch_size,
        "epochs": args.epochs,
        "seed": args.seed,
        "best_epoch": best_epoch,
        "best_val_loss": best_val,
        "final_val_mae_cp": history[-1]["val_mae_cp"],
        "best_val_mae_cp": min(h["val_mae_cp"] for h in history),
        "buckets_best": min(history, key=lambda h: h["val_loss"])["buckets"],
        "compute": {
            "device": str(device),
            "gpu": gpu_name,
            "total_seconds": round(total_seconds, 1),
            "steps": steps_total,
            "train_examples": train_examples,
            "examples_per_second": round(train_examples / max(total_seconds, 1e-9), 1),
        },
        "checkpoint": args.output,
        "history": history,
    }
    if args.curve_out:
        os.makedirs(os.path.dirname(args.curve_out) or ".", exist_ok=True)
        with open(args.curve_out, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)
        print(f"wrote curve to {args.curve_out}")
    if args.holdout:
        holdout_path = args.curve_out.replace(".json", "_holdout.json") if args.curve_out \
            else "holdout.json"
        with open(holdout_path, "w", encoding="utf-8") as f:
            json.dump({"dataset_checksum": metadata.get("checksum"),
                       "records": valid_records}, f)
        print(f"wrote holdout to {holdout_path}")

    print(f"saved {args.output} (best epoch {best_epoch}, "
          f"val_loss={best_val:.4f}, mae={summary['best_val_mae_cp']:.0f}cp)")


if __name__ == "__main__":
    main()