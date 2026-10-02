"""Reference move-tree enumeration, matching the engine's `perft --tree`.

Emits `seq,count` lines where `seq` is the concatenated UCI moves from the root
and `count` is the number of legal moves at that node. Diffing against
`perft.exe --tree` pinpoints the first diverging move sequence.

    py -3 tools/reference_tree.py --case kiwipete --depth 3 > ref.csv
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


def walk(board: chess.Board, depth: int, path: str, out) -> None:
    moves = list(board.legal_moves)
    if depth == 0:
        out.append(f"{path},{len(moves)}")
        return
    for mv in moves:
        board.push(mv)
        walk(board, depth - 1, path + mv.uci(), out)
        board.pop()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fen")
    ap.add_argument("--case", default="startpos", choices=sorted(CASES))
    ap.add_argument("--depth", type=int, default=3)
    args = ap.parse_args()

    board = chess.Board(args.fen or CASES[args.case])
    out: list[str] = []
    walk(board, args.depth, "", out)
    print("\n".join(out))


if __name__ == "__main__":
    main()
