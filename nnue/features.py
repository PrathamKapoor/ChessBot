"""Pocket Chess NNUE feature representation.

HalfKP-style features: for the side to move, each (king_square, piece) pair
active pieces (including the opponent's king) generate one feature.

Horizontal mirroring: when the side-to-move king is on the kingside (files
e-h), the board is mirrored so the king always appears on files a-d. This
halves the effective king-square range.

Feature index layout:
    feature_idx = (mirrored_king_sq * 12 + piece_type_color_idx) * 64 + piece_sq

where piece_type_color_idx is 0..11 for white pawn..black king, and
mirrored_king_sq is the rank-file index after mirroring.

Total features: 64 * 12 * 64 = 49,152.

Only the side-to-move king is used for indexing; it is NOT itself a feature.
All other pieces (including the opponent's king) are features.
"""

import chess

# piece_type_color_idx: 0=white pawn, 1=white knight, ..., 5=white king,
#                        6=black pawn, ..., 11=black king
def _piece_index(piece: chess.Piece) -> int:
    return (0 if piece.color == chess.WHITE else 6) + (piece.piece_type - 1)

def _mirror_sq(sq: int) -> int:
    f, r = chess.square_file(sq), chess.square_rank(sq)
    return chess.square(7 - f, r)

def _mirror_king_sq(king_sq: int) -> int:
    f, r = chess.square_file(king_sq), chess.square_rank(king_sq)
    if f >= 4:
        f = 7 - f
    return chess.square(f, r)

def _mirror_board(board: chess.Board) -> chess.Board:
    """Return a new board with all squares mirrored horizontally."""
    new = board.copy(stack=False)
    # mirror pieces
    pieces = {}
    for sq in chess.SQUARES:
        p = board.piece_at(sq)
        if p:
            pieces[sq] = p
    new.clear()
    for sq, p in pieces.items():
        new.set_piece_at(_mirror_sq(sq), p)
    new.turn = board.turn
    new.castling_rights = board.castling_rights  # not exact but irrelevant for features
    new.ep_square = _mirror_sq(board.ep_square) if board.ep_square is not None else None
    return new

def active_features(board: chess.Board) -> list[int]:
    """Return the list of active feature indices for the given position."""
    side = board.turn
    king_sq = board.king(side)
    if king_sq is None:
        return []

    # Mirror if the king is on the kingside
    mirror = chess.square_file(king_sq) >= 4
    if mirror:
        board = _mirror_board(board)
        king_sq = _mirror_king_sq(king_sq)

    out = []
    for sq in chess.SQUARES:
        p = board.piece_at(sq)
        if p is None:
            continue
        if p.piece_type == chess.KING and p.color == side:
            continue  # active king is used for indexing, not as a feature
        pi = _piece_index(p)
        out.append(king_sq * 768 + pi * 64 + sq)
    return out


FEATURE_COUNT = 49152  # 64 * 12 * 64


def sanity_check():
    b = chess.Board()
    feats = active_features(b)
    # Expect 30 features (32 total pieces minus 2 kings of side to move... wait,
    # only side-to-move king excluded, so 31 pieces minus own king = 30?
    # Actually: 16 white + 16 black = 32. Side to move king excluded = 31.
    # But in HalfKP we exclude only the side-to-move king, so 31 features.
    # Hmm, but both kings exist on the board. Let me recount.
    # White king excluded (side to move is white): features for 32-1 = 31 pieces.
    print(f"startpos features: {len(feats)}")
    assert len(feats) == 31, f"expected 31, got {len(feats)}"
    print("sanity check OK")


if __name__ == "__main__":
    sanity_check()
