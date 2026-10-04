"""Train the NNUE model on a dataset."""

import argparse
import json
import os
import random
import sys
import time

import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader

import chess

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from features import active_features
from model import NNUEModel, SCALE

TARGET_CLIP = 2000.0  # clip teacher scores to +/- this before scaling


class NNUEDataset(Dataset):
    def __init__(self, records, max_score=TARGET_CLIP):
        self.max_score = max_score
        # Features are pure functions of the FEN: compute once instead of
        # re-running python-chess every epoch (dominates epoch time otherwise).
        self.feats = [active_features(chess.Board(r["fen"])) for r in records]
        self.targets = torch.tensor(
            [max(-max_score, min(max_score, r["score_cp"])) / SCALE
             for r in records],
            dtype=torch.float32)

    def __len__(self):
        return len(self.targets)

    def __getitem__(self, idx):
        return self.feats[idx], self.targets[idx]


def collate(batch):
    feats = [f for f, _ in batch]
    targets = torch.stack([t for _, t in batch])
    return feats, targets


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
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    random.seed(args.seed)

    with open(args.data, "r", encoding="utf-8") as f:
        raw = json.load(f)
    records = raw["records"]
    random.shuffle(records)

    split = int(len(records) * 0.8)
    train_records = records[:split]
    val_records = records[split:]
    print(f"train={len(train_records)} val={len(val_records)}")

    t0 = time.time()
    train_ds = NNUEDataset(train_records)
    val_ds = NNUEDataset(val_records)
    print(f"features precomputed in {time.time() - t0:.1f}s")

    train_loader = DataLoader(train_ds, batch_size=args.batch_size, shuffle=True, collate_fn=collate)
    val_loader = DataLoader(val_ds, batch_size=args.batch_size, shuffle=False, collate_fn=collate)

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"device: {device}")
    model = NNUEModel().to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr,
                                  weight_decay=args.weight_decay)
    milestones = [int(m) for m in args.lr_milestones.split(",") if m.strip()]
    scheduler = torch.optim.lr_scheduler.MultiStepLR(optimizer, milestones=milestones,
                                                     gamma=args.lr_gamma)
    loss_fn = nn.HuberLoss()

    best_val = float("inf")
    for epoch in range(args.epochs):
        t0 = time.time()
        model.train()
        total_loss = 0.0
        for feats, targets in train_loader:
            targets = targets.to(device)
            optimizer.zero_grad()
            pred = model(feats)
            loss = loss_fn(pred, targets)
            loss.backward()
            optimizer.step()
            total_loss += loss.item() * len(feats)

        model.eval()
        val_loss = 0.0
        mae = 0.0
        with torch.no_grad():
            for feats, targets in val_loader:
                targets = targets.to(device)
                pred = model(feats)
                val_loss += loss_fn(pred, targets).item() * len(feats)
                mae += torch.abs(pred - targets).sum().item()

        scheduler.step()
        train_loss = total_loss / len(train_records)
        val_loss = val_loss / len(val_records)
        val_mae = mae / len(val_records)
        lr = optimizer.param_groups[0]["lr"]
        marker = ""
        if val_loss < best_val:
            best_val = val_loss
            marker = " *"
            os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
            torch.save({
                "state_dict": model.state_dict(),
                "config": {"feature_dim": model.feature_dim, "hidden": model.hidden,
                           "layers": model.layers},
                "dataset": args.data,
                "dataset_checksum": raw["metadata"].get("checksum"),
                "seed": args.seed,
                "epochs": args.epochs,
                "epoch": epoch + 1,
                "val_loss": val_loss,
                "best_val_loss": best_val,
            }, args.output)
        print(f"epoch {epoch+1:3d}  train_loss={train_loss:.4f}  val_loss={val_loss:.4f}  "
              f"val_mae={val_mae:.4f}  lr={lr:g}  {time.time()-t0:.0f}s{marker}")

    print(f"saved checkpoint to {args.output} (best val_loss={best_val:.4f})")


if __name__ == "__main__":
    main()
