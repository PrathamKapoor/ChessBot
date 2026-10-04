"""Pocket Chess NNUE model in PyTorch.

Architecture: HalfKP-style sparse features -> big feature transformer ->
ReLU -> small dense -> scalar output.

The feature transformer accumulates active features into a dense vector.
The small network maps that vector to a scalar evaluation.
"""

import torch
import torch.nn as nn

FEATURE_DIM = 49152
HIDDEN = 256
LAYERS = 2
SCALE = 400.0  # teacher scores are clipped/scaled by this


class NNUEModel(nn.Module):
    def __init__(self, feature_dim=FEATURE_DIM, hidden=HIDDEN, layers=LAYERS):
        super().__init__()
        self.feature_dim = feature_dim
        self.hidden = hidden
        self.layers = layers

        # Feature transformer: maps a sparse feature set to a dense accumulator.
        # Implemented as a linear layer but only indexed by active features.
        self.ft_weight = nn.Parameter(torch.randn(hidden, feature_dim) * 0.01)
        self.ft_bias = nn.Parameter(torch.zeros(hidden))

        # Downstream layers
        dims = [hidden]
        for _ in range(layers):
            dims.append(32)
        self.fc1 = nn.Linear(dims[0], dims[1])
        self.fc2 = nn.Linear(dims[1], 1) if layers == 1 else nn.Linear(dims[1], dims[2])
        if layers == 2:
            self.fc3 = nn.Linear(dims[2], 1)
        else:
            self.fc3 = None

    def forward(self, active_features: list[list[int]]) -> torch.Tensor:
        """
        active_features: batch of lists of feature indices.
        Returns: tensor of shape (batch,) with scalar evaluations.
        """
        batch = len(active_features)
        # Build sparse accumulator
        acc = self.ft_bias.unsqueeze(0).expand(batch, -1).clone()
        for i, feats in enumerate(active_features):
            if feats:
                acc[i] = acc[i] + self.ft_weight[:, feats].sum(dim=1)
        # Downstream
        x = torch.relu(acc)
        x = torch.relu(self.fc1(x))
        if self.fc3 is not None:
            x = torch.relu(self.fc2(x))
            x = self.fc3(x).squeeze(-1)
        else:
            x = self.fc2(x).squeeze(-1)
        return x


def make_model():
    return NNUEModel()


def count_parameters(m: nn.Module) -> int:
    return sum(p.numel() for p in m.parameters())


if __name__ == "__main__":
    m = make_model()
    print(f"parameters: {count_parameters(m):,}")
    import chess
    from features import active_features
    b = chess.Board()
    feats = active_features(b)
    out = m([feats])
    print(f"startpos output: {out.item():.4f}")
