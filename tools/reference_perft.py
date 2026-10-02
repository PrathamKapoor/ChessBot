"""Reference perft using python-chess, used to bisect move generation bugs.

Prints, for a given FEN and depth, one line per root move:

    <move> <count>

The engine's own `perft --divides` output can be diffed against this to find
exactly which moves are missing or over-counted.

Usage:
    py -3 tools/reference_perft.py --depth 2
    py -3 tools/reference_perft.py --depth 3 --case kiwipete
"""
import argparse
import chess

CASES = {
    "startpos": "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "kiwipete": "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "endgame-rook": "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "promo-castle": "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "promo-heavy": "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "positional": "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
}


def perft(board: chess.Board, depth: int) -> int:
    if depth <= 0:
        return 1
    if depth == 1:
        return board.legal_moves.count()
    total = 0
    for mv in board.legal_moves:
        board.push(mv)
        total += perft(board, depth - 1)
        board.pop()
    return total


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fen")
    ap.add_argument("--case", default="startpos", choices=sorted(CASES))
    ap.add_argument("--depth", type=int, default=2)
    args = ap.parse_args()

    fen = args.fen or CASES[args.case]
    board = chess.Board(fen)

    # Per-depth root divides, in the same shape as `perft --divides`.
    for d in range(1, args.depth + 1):
        print(f"=== depth {d} ===")
        for mv in board.legal_moves:
            board.push(mv)
            sub = perft(board, d - 1)
            board.pop()
            print(f"  {mv.uci()} : {sub}")


if __name__ == "__main__":
    main()
