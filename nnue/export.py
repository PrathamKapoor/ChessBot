"""Export a trained NNUE checkpoint to a simple binary format for C++ inference."""

import argparse
import json
import struct

import torch

sys_path = __file__.replace("export.py", "")
import os
import sys
sys.path.insert(0, sys_path)
from model import NNUEModel


def export(checkpoint_path, output_path):
    ckpt = torch.load(checkpoint_path, map_location="cpu", weights_only=False)
    cfg = ckpt["config"]
    model = NNUEModel(cfg["feature_dim"], cfg["hidden"], cfg["layers"])
    model.load_state_dict(ckpt["state_dict"])
    model.eval()

    with open(output_path, "wb") as f:
        # Header
        f.write(b"PCNN")  # magic
        f.write(struct.pack("<I", 1))  # version
        f.write(struct.pack("<I", cfg["feature_dim"]))
        f.write(struct.pack("<I", cfg["hidden"]))
        f.write(struct.pack("<I", cfg["layers"]))

        # Feature transformer weight: (hidden, feature_dim) -> stored in sparse
        # For simplicity, store dense for now
        ft_w = ckpt["state_dict"]["ft_weight"].numpy()
        ft_b = ckpt["state_dict"]["ft_bias"].numpy()
        f.write(ft_w.tobytes())
        f.write(ft_b.tobytes())

        # Downstream layers
        fc1_w = ckpt["state_dict"]["fc1.weight"].numpy()
        fc1_b = ckpt["state_dict"]["fc1.bias"].numpy()
        f.write(fc1_w.tobytes())
        f.write(fc1_b.tobytes())

        if cfg["layers"] == 2:
            fc2_w = ckpt["state_dict"]["fc2.weight"].numpy()
            fc2_b = ckpt["state_dict"]["fc2.bias"].numpy()
            fc3_w = ckpt["state_dict"]["fc3.weight"].numpy()
            fc3_b = ckpt["state_dict"]["fc3.bias"].numpy()
            f.write(fc2_w.tobytes())
            f.write(fc2_b.tobytes())
            f.write(fc3_w.tobytes())
            f.write(fc3_b.tobytes())
        else:
            fc2_w = ckpt["state_dict"]["fc2.weight"].numpy()
            fc2_b = ckpt["state_dict"]["fc2.bias"].numpy()
            f.write(fc2_w.tobytes())
            f.write(fc2_b.tobytes())

    print(f"exported to {output_path}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    export(args.checkpoint, args.output)
