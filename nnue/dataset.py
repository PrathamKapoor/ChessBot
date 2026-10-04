"""Generate a NNUE training dataset using the classical engine as teacher.

Position sources (mixed by default):
  - self-play: the frozen classical engine plays itself at low depth, so the
    positions match the distribution the NNUE will actually search.
  - random playouts: legal but artificial games, kept for diversity.

Every harvested position is labelled with the teacher's search score at
--depth. Generation parallelises across --workers engine processes.
"""

import argparse
import hashlib
import json
import os
import random
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import chess
import chess.engine

ENGINE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "build", "pocketchess.exe")


def engine_eval(fen: str, depth: int, timeout: float | None = None) -> float | None:
    """Run the engine on a position and return the score in cp, or None."""
    if timeout is None:
        timeout = depth + 10
    try:
        p = subprocess.run(
            [ENGINE],
            input=f"uci\nisready\nposition fen {fen}\ngo depth {depth}\nquit\n",
            capture_output=True, text=True, timeout=timeout,
        )
    except Exception:
        return None
    score = None
    for line in p.stdout.splitlines():
        if line.startswith("info") and "score" in line:
            parts = line.split()
            if "cp" in parts:
                idx = parts.index("cp")
                score = float(parts[idx + 1])
            elif "mate" in parts:
                idx = parts.index("mate")
                mate_in = int(parts[idx + 1])
                score = 30000.0 if mate_in > 0 else -30000.0
    return score


def random_game_fens(rng: random.Random, min_ply: int, harvest_every: int,
                     max_plies: int = 120) -> list[str]:
    board = chess.Board()
    fens = []
    plies = 0
    while plies < max_plies and not board.is_game_over():
        moves = list(board.legal_moves)
        if not moves:
            break
        board.push(rng.choice(moves))
        plies += 1
        if plies >= min_ply and (plies - min_ply) % harvest_every == 0:
            fens.append(board.fen())
        if board.is_game_over():
            break
    return fens


def selfplay_game_fens(engine: chess.engine.SimpleEngine, depth: int, rng: random.Random,
                       min_ply: int, harvest_every: int, max_plies: int = 120) -> list[str]:
    board = chess.Board()
    # The engine at fixed depth is deterministic, so without a random opening
    # every worker replays the same game and the dataset collapses on dedup.
    for _ in range(rng.randint(4, 10)):
        moves = list(board.legal_moves)
        if not moves:
            break
        board.push(rng.choice(moves))
        if board.is_game_over():
            break
    fens = []
    plies = board.ply()
    while plies < max_plies and not board.is_game_over():
        if plies >= min_ply and (plies - min_ply) % harvest_every == 0:
            fens.append(board.fen())
        try:
            result = engine.play(board, chess.engine.Limit(depth=depth, time=2.0))
        except Exception:
            break
        if result.move is None:
            break
        board.push(result.move)
        plies += 1
    return fens


def selfplay_worker(games: int, depth: int, seed: int, min_ply: int,
                    harvest_every: int) -> list[str]:
    rng = random.Random(seed)
    engine = chess.engine.SimpleEngine.popen_uci(ENGINE)
    fens: list[str] = []
    try:
        for _ in range(games):
            fens.extend(selfplay_game_fens(engine, depth, rng, min_ply, harvest_every))
    finally:
        engine.quit()
    return fens


def random_worker(games: int, seed: int, min_ply: int, harvest_every: int) -> list[str]:
    rng = random.Random(seed)
    fens: list[str] = []
    for _ in range(games):
        fens.extend(random_game_fens(rng, min_ply, harvest_every))
    return fens


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", required=True)
    ap.add_argument("--selfplay-games", type=int, default=400)
    ap.add_argument("--random-games", type=int, default=400)
    ap.add_argument("--source", choices=["mix", "selfplay", "random"], default="mix")
    ap.add_argument("--depth", type=int, default=6, help="teacher search depth")
    ap.add_argument("--selfplay-depth", type=int, default=3)
    ap.add_argument("--min-ply", type=int, default=8)
    ap.add_argument("--harvest-every", type=int, default=3)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--workers", type=int, default=8)
    args = ap.parse_args()

    if args.source == "selfplay":
        args.random_games = 0
    elif args.source == "random":
        args.selfplay_games = 0

    rng = random.Random(args.seed)
    t0 = time.time()
    print(f"Generating positions: {args.selfplay_games} self-play games (depth "
          f"{args.selfplay_depth}), {args.random_games} random games (seed {args.seed})...",
          flush=True)

    rand_seeds = [args.seed + 2000 * (i + 1) for i in range(args.random_games)]
    # Chunk games per worker so each worker reuses one engine process.
    def chunks(lst, n):
        return [lst[i::n] for i in range(n)]

    fens: list[str] = []
    workers = max(1, args.workers)
    if args.selfplay_games:
        per_worker = [args.selfplay_games // workers + (1 if i < args.selfplay_games % workers else 0)
                      for i in range(workers)]
        with ThreadPoolExecutor(max_workers=workers) as ex:
            futs = [ex.submit(selfplay_worker, n, args.selfplay_depth,
                              args.seed + 997 * (i + 1), args.min_ply, args.harvest_every)
                    for i, n in enumerate(per_worker) if n]
            for f in futs:
                fens.extend(f.result())
    if args.random_games:
        with ThreadPoolExecutor(max_workers=workers) as ex:
            futs = [ex.submit(random_worker, len(c), c[0], args.min_ply, args.harvest_every)
                    for c in chunks(rand_seeds, workers) if c]
            for f in futs:
                fens.extend(f.result())

    rng.shuffle(fens)
    # Deduplicate while preserving the shuffled order.
    seen = set()
    unique_fens = []
    for fen in fens:
        if fen not in seen:
            seen.add(fen)
            unique_fens.append(fen)
    print(f"Harvested {len(fens)} positions ({len(unique_fens)} unique) "
          f"in {time.time()-t0:.0f}s", flush=True)
    t1 = time.time()

    print(f"Evaluating with teacher at depth {args.depth} on {workers} workers...",
          flush=True)

    def label(fen: str):
        return engine_eval(fen, args.depth)

    scores: list[float | None] = [None] * len(unique_fens)
    done = 0
    with ThreadPoolExecutor(max_workers=workers) as ex:
        futs = {ex.submit(label, fen): i for i, fen in enumerate(unique_fens)}
        for fut in as_completed(futs):
            scores[futs[fut]] = fut.result()
            done += 1
            if done % 1000 == 0:
                rate = done / max(time.time() - t1, 1e-9)
                eta = (len(unique_fens) - done) / max(rate, 1e-9)
                print(f"  {done}/{len(unique_fens)} labelled "
                      f"({rate:.0f}/s, eta {eta:.0f}s)", flush=True)

    records = [{"fen": fen, "score_cp": score, "depth": args.depth}
               for fen, score in zip(unique_fens, scores) if score is not None]
    dropped = len(unique_fens) - len(records)

    checksum = hashlib.sha256(
        json.dumps(records, sort_keys=True).encode()
    ).hexdigest()[:16]

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump({"metadata": {
            "positions": len(records),
            "dropped": dropped,
            "depth": args.depth,
            "selfplay_games": args.selfplay_games,
            "selfplay_depth": args.selfplay_depth,
            "random_games": args.random_games,
            "harvest_every": args.harvest_every,
            "min_ply": args.min_ply,
            "seed": args.seed,
            "checksum": checksum,
            "engine": ENGINE,
        }, "records": records}, f, indent=2)

    print(f"Wrote {len(records)} records ({dropped} evals dropped) to {args.output}",
          flush=True)
    print(f"Checksum: {checksum}", flush=True)


if __name__ == "__main__":
    main()
