"""Dataset generator, format v3.

Format v2 (Phase 6) stored only {fen, score_cp, depth}, which made two things
impossible: a game-level train/validation split, and any sampling policy other
than "everything". Format v3 adds provenance and category metadata to every
record so that later experiments can be run as subsets and reweightings of one
immutable pool:

  game_id     stable identifier of the source game (split by this, never by
              record, so positions a few plies apart cannot straddle the split)
  source      selfplay | random
  ply         ply number inside the source game
  depth       teacher depth used for score_cp
  score_cp    teacher score in centipawns, side to move, +/-30000 for mate
  shallow_cp  optional label from a shallower search, used as an evaluation
              volatility proxy for tactical sampling
  phase       opening_middlegame | late_middlegame | endgame
  flags       reproducible tactical indicators (in_check, capture_available,
              promotion_available, decisive)
  endgame     endgame class labels (pawnless, pawn_endgame, minor_ending,
              rook_ending, queen_ending, sparse_pieces, opposite_bishops)

The teacher is the frozen classical engine. Labelling uses one persistent UCI
engine per worker with `ucinewgame` before every position, which keeps each
label independent of the transposition table, exactly as a fresh process did.

Every phase is sharded to disk. The harvest goes to `<output>.shards/harvest.json`
and each labelling chunk to `<output>.shards/chunk_NNNN.json` the moment that
chunk finishes, so an interrupted run resumes with `--resume` instead of
starting over - the first version wrote only at the end, and a machine restart
mid-labelling cost a whole v4-pool run and a whole depth-9 relabel. Chunk
shards also fix the record order: records are assembled in chunk order, not
completion order, so the checksum does not depend on thread timing.

Example:
    py -3 -u nnue/generate_v3.py --output nnue/data/v3_pool.json \\
        --selfplay-games 2400 --random-games 400 --depth 7 \\
        --shallow-depth 3 --workers 14 --seed 42
"""

import argparse
import hashlib
import json
import os
import random
import shutil
import subprocess
import sys
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed

import chess
import chess.engine

GENERATOR_VERSION = 4
# The record schema did not change since v3 (see the docstring); FORMAT pins the
# schema and GENERATOR_VERSION the script's behaviour (sharded resume, dedupe
# key). Both go into the metadata and into the shard config comparison, so a
# --resume can never silently mix two generator behaviours.
FORMAT = 3
ENGINE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "build", "pocketchess.exe")

PIECE_VALUE = {chess.PAWN: 100, chess.KNIGHT: 320, chess.BISHOP: 330,
               chess.ROOK: 500, chess.QUEEN: 900, chess.KING: 0}


def phase_of(board):
    pieces = list(board.piece_map().values())
    total = len(pieces)
    non_pawn = sum(1 for p in pieces if p.piece_type not in (chess.PAWN, chess.KING))
    if total <= 6 or non_pawn <= 2:
        return "endgame"
    if non_pawn <= 6 or total <= 14:
        return "late_middlegame"
    return "opening_middlegame"


def has_opposite_bishops(board):
    dark, light = [], []
    for square, piece in board.piece_map().items():
        if piece.piece_type != chess.BISHOP:
            continue
        is_dark = bool(chess.BB_DARK_SQUARES & chess.BB_SQUARES[square])
        (dark if is_dark else light).append(piece.color)
    return len(dark) == 1 and len(light) == 1 and dark[0] != light[0]


def endgame_labels(board):
    labels = []
    counts = Counter(p.piece_type for p in board.piece_map().values())
    non_pawn = sum(counts.get(pt, 0) for pt in
                   (chess.KNIGHT, chess.BISHOP, chess.ROOK, chess.QUEEN))
    if counts.get(chess.PAWN, 0) == 0:
        labels.append("pawnless")
    if non_pawn == 0:
        labels.append("pawn_endgame")
    elif non_pawn <= 2:
        labels.append("minor_ending")
    elif non_pawn <= 4:
        labels.append("rook_ending")
    elif non_pawn <= 8 and counts.get(chess.QUEEN, 0) <= 1:
        labels.append("queen_ending")
    if non_pawn + 2 <= 8:
        labels.append("sparse_pieces")
    if has_opposite_bishops(board):
        labels.append("opposite_bishops")
    return labels


def position_tags(board):
    flags = []
    if board.is_check():
        flags.append("in_check")
    legal = list(board.legal_moves)
    if any(board.is_capture(m) for m in legal):
        flags.append("capture_available")
    if any(m.promotion for m in legal):
        flags.append("promotion_available")
    if any(board.gives_check(m) for m in legal):
        flags.append("check_available")
    return flags


def label_position(board, depths, teacher):
    """Return (primary_cp, shallow_cp) for one position from one engine.

    The transposition table is cleared before every position so a label depends
    only on the position and the depth, exactly as a fresh process did.
    """
    values = {}
    for depth in depths:
        try:
            teacher.protocol.send_line("ucinewgame")
            result = teacher.play(board, chess.engine.Limit(depth=depth),
                                  info=chess.engine.INFO_ALL)
        except Exception:
            return None, None
        score = result.info.get("score") if result.info else None
        if score is None:
            return None, None
        relative = score.relative
        if relative is None:
            continue
        if relative.is_mate():
            values[depth] = 30000.0 if relative.mate() > 0 else -30000.0
        else:
            values[depth] = float(relative.score())
    return values.get(depths[-1]), values.get(depths[0])


def harvest_selfplay(engine, depth, rng, game_id, min_ply, harvest_every,
                     harvest_every_late, late_from_ply, max_plies, opening_moves):
    board = chess.Board()
    for _ in range(rng.randint(opening_moves[0], opening_moves[1])):
        moves = list(board.legal_moves)
        if not moves:
            break
        board.push(rng.choice(moves))
        if board.is_game_over():
            break
    out = []
    ply = board.ply()
    while ply < max_plies and not board.is_game_over():
        step = harvest_every_late if ply >= late_from_ply else harvest_every
        if ply >= min_ply and (ply - min_ply) % step == 0:
            out.append((board.fen(), game_id, "selfplay", ply))
        try:
            result = engine.play(board, chess.engine.Limit(depth=depth))
        except Exception:
            break
        if result.move is None:
            break
        board.push(result.move)
        ply += 1
    return out


def harvest_random(rng, game_id, min_ply, harvest_every, harvest_every_late,
                   late_from_ply, max_plies):
    board = chess.Board()
    out = []
    ply = 0
    while ply < max_plies and not board.is_game_over():
        moves = list(board.legal_moves)
        if not moves:
            break
        board.push(rng.choice(moves))
        ply += 1
        step = harvest_every_late if ply >= late_from_ply else harvest_every
        if ply >= min_ply and (ply - min_ply) % step == 0:
            out.append((board.fen(), game_id, "random", ply))
    return out


def git_commit():
    try:
        return subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True,
                              text=True, cwd=os.path.dirname(ENGINE)).stdout.strip()
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", required=True)
    ap.add_argument("--selfplay-games", type=int, default=2400)
    ap.add_argument("--random-games", type=int, default=400)
    ap.add_argument("--depth", type=int, default=7, help="teacher depth for score_cp")
    ap.add_argument("--shallow-depth", type=int, default=0,
                    help="optional shallow depth, stored as shallow_cp (0 = off)")
    ap.add_argument("--selfplay-depth", type=int, default=3)
    ap.add_argument("--min-ply", type=int, default=8)
    ap.add_argument("--harvest-every", type=int, default=3)
    ap.add_argument("--harvest-every-late", type=int, default=2,
                    help="harvest interval after --late-from-ply (endgame skew)")
    ap.add_argument("--late-from-ply", type=int, default=40)
    ap.add_argument("--max-plies", type=int, default=140)
    ap.add_argument("--opening-min", type=int, default=4)
    ap.add_argument("--opening-max", type=int, default=10)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--label-timeout", type=float, default=None)
    ap.add_argument("--resume", action="store_true",
                    help="reuse the shards of an interrupted run of this exact "
                         "configuration")
    ap.add_argument("--clean-shards", action="store_true",
                    help="delete the shards directory after the dataset is written")
    args = ap.parse_args()

    # The harvest stride must be odd. An even stride never changes the side to
    # move, so every position harvested past `late_from_ply` comes from the same
    # colour, and since ply parity fixes which colour that is, the dense endgame
    # harvest produces an all-white endgame block. That is exactly what the
    # first v3 pool did: --harvest-every-late 2 gave 268,425 white to move
    # against 32,141 black, with 37,836 endgames of which zero were black to
    # move. The failure is silent because the file looks healthy and the
    # checksum verifies. Refuse even strides rather than reproduce it.
    for name, value in (("--harvest-every", args.harvest_every),
                        ("--harvest-every-late", args.harvest_every_late)):
        if value % 2 == 0:
            raise SystemExit(
                f"{name}={value} is even; an even harvest stride cannot change "
                "the side to move, so all late-game positions would share one "
                "colour. Use an odd stride (1 or 3).")

    if args.label_timeout is None:
        # Depth 11 on a single quiet position can take many seconds; keep a
        # generous ceiling so slow positions are dropped, not stalled.
        args.label_timeout = 2.0 * (2 ** max(0, args.depth - 6)) * 3 + 20

    t0 = time.time()
    commit = git_commit()
    print(f"generate_v3 output={args.output} selfplay={args.selfplay_games} "
          f"random={args.random_games} depth={args.depth} "
          f"shallow={args.shallow_depth} workers={args.workers} seed={args.seed}",
          flush=True)
    print(f"generator commit {commit}", flush=True)

    # ---- shards: every phase survives a kill -----------------------------
    shards_dir = args.output + ".shards"
    config_path = os.path.join(shards_dir, "config.json")
    harvest_path = os.path.join(shards_dir, "harvest.json")
    stored_config = {"generator_version": GENERATOR_VERSION, "format": FORMAT,
                     **{k: v for k, v in vars(args).items()
                        if k not in ("resume", "clean_shards")}}

    def write_json(path, payload, indent=None):
        # Write-then-rename so a kill cannot leave a half-written shard that a
        # later --resume would load as if it were complete.
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=indent)
        os.replace(tmp, path)

    resumed_chunks = 0
    if os.path.isdir(shards_dir) and os.listdir(shards_dir):
        if not args.resume:
            raise SystemExit(
                f"{shards_dir} exists. Refusing to start a fresh run on top of "
                "an interrupted one; pass --resume to continue it, or move the "
                "directory aside.")
        with open(config_path, "r", encoding="utf-8") as f:
            previous = json.load(f)
        if previous != stored_config:
            differing = sorted(k for k in stored_config
                               if previous.get(k) != stored_config[k])
            raise SystemExit(
                f"{shards_dir} was written by a different configuration "
                f"({', '.join(differing)}); refusing to resume. Move the "
                "directory aside to start over.")
        print(f"resuming from {shards_dir}", flush=True)
    else:
        os.makedirs(shards_dir, exist_ok=True)
        write_json(config_path, stored_config)

    workers = max(1, args.workers)
    harvest: list[tuple[str, str, str, int]] = []

    # ---- phase 1: harvest positions -------------------------------------
    def selfplay_job(index):
        rng = random.Random(args.seed + 9973 * (index + 1))
        engine = chess.engine.SimpleEngine.popen_uci(ENGINE)
        game_id = f"sp{index:06d}"
        try:
            return harvest_selfplay(engine, args.selfplay_depth, rng, game_id,
                                    args.min_ply, args.harvest_every,
                                    args.harvest_every_late, args.late_from_ply,
                                    args.max_plies,
                                    (args.opening_min, args.opening_max))
        finally:
            # close() terminates the subprocess; quit() alone can leave the
            # engine alive holding this process's stdout pipe.
            try:
                engine.close()
            except Exception:
                pass

    def random_job(index):
        rng = random.Random(args.seed + 20011 * (index + 1))
        game_id = f"rd{index:06d}"
        return harvest_random(rng, game_id, args.min_ply, args.harvest_every,
                              args.harvest_every_late, args.late_from_ply,
                              args.max_plies)

    if os.path.exists(harvest_path):
        with open(harvest_path, "r", encoding="utf-8") as f:
            saved = json.load(f)
        harvest = [tuple(item) for item in saved["positions"]]
        harvest_count = saved["harvested"]
        print(f"harvest restored from {harvest_path}: {len(harvest)} unique "
              f"of {harvest_count} positions", flush=True)
    else:
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futs = []
            for i in range(args.selfplay_games):
                futs.append(pool.submit(selfplay_job, i))
            for i in range(args.random_games):
                futs.append(pool.submit(random_job, i))
            done = 0
            for fut in as_completed(futs):
                harvest.extend(fut.result())
                done += 1
                if done % 100 == 0:
                    print(f"  harvested {done}/{len(futs)} games, "
                          f"{len(harvest)} positions ({time.time()-t0:.0f}s)",
                          flush=True)

        rng = random.Random(args.seed + 999983)
        rng.shuffle(harvest)
        seen = set()
        unique = []
        for item in harvest:
            # Dedupe on the position without move counters. Two FENs that differ
            # only in the halfmove/fullmove counters have identical HalfKP
            # features and identical teacher labels, so keeping both would just
            # double-weight that sample. diagnose.py measured the collapse at
            # 0.3% for the v1/v2 generator and 1.1% for the first v3 pool.
            key = " ".join(item[0].split(" ")[:4])
            if key in seen:
                continue
            seen.add(key)
            unique.append(item)
        harvest_count = len(harvest)
        write_json(harvest_path, {"harvested": harvest_count,
                                  "positions": unique})
        print(f"harvested {harvest_count} positions ({len(unique)} unique) in "
              f"{time.time()-t0:.0f}s", flush=True)

    unique = [tuple(item) for item in harvest]
    len_unique = len(unique)

    # ---- phase 2: label ---------------------------------------------------
    depths = [args.depth]
    if args.shallow_depth:
        depths.append(args.shallow_depth)

    def label_job(chunk):
        teacher = chess.engine.SimpleEngine.popen_uci(ENGINE)
        out = []
        try:
            for fen, game_id, source, ply in chunk:
                board = chess.Board(fen)
                primary, shallow = label_position(board, depths, teacher)
                if primary is None:
                    continue
                out.append({
                    "fen": fen,
                    "score_cp": primary,
                    "depth": args.depth,
                    "game_id": game_id,
                    "source": source,
                    "ply": ply,
                    "phase": phase_of(board),
                    "flags": position_tags(board),
                    "endgame": endgame_labels(board),
                })
                if shallow is not None:
                    out[-1]["shallow_cp"] = shallow
                    out[-1]["shallow_depth"] = args.shallow_depth
        finally:
            try:
                teacher.close()
            except Exception:
                pass
        return out

    chunk_size = max(1, len(unique) // (workers * 8) + 1)
    chunks = [unique[i:i + chunk_size] for i in range(0, len(unique), chunk_size)]

    def chunk_path(index):
        return os.path.join(shards_dir, f"chunk_{index:04d}.json")

    def load_chunk(index):
        with open(chunk_path(index), "r", encoding="utf-8") as f:
            return json.load(f)

    pending = [i for i in range(len(chunks)) if not os.path.exists(chunk_path(i))]
    resumed_chunks = len(chunks) - len(pending)
    labelled_before = (sum(len(load_chunk(i)) for i in range(len(chunks))
                           if os.path.exists(chunk_path(i)))
                       if resumed_chunks else 0)
    if resumed_chunks:
        print(f"labelling: {resumed_chunks}/{len(chunks)} chunks already on disk "
              f"({labelled_before} records); {len(pending)} to run", flush=True)

    t1 = time.time()
    newly_labelled = 0
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futs = {pool.submit(label_job, chunks[i]): i for i in pending}
        done = resumed_chunks
        for fut in as_completed(futs):
            index = futs[fut]
            chunk_records = fut.result()
            write_json(chunk_path(index), chunk_records)
            done += 1
            newly_labelled += len(chunk_records)
            newly_done = done - resumed_chunks
            rate = newly_labelled / max(time.time() - t1, 1e-9)
            eta = (time.time() - t1) / max(newly_done, 1) * (len(pending) - newly_done)
            print(f"  chunk {done}/{len(chunks)}: "
                  f"{labelled_before + newly_labelled}/{len(unique)} labelled "
                  f"({rate:.1f}/s, eta {eta/60:.1f} min)", flush=True)

    records = []
    for index in range(len(chunks)):
        if not os.path.exists(chunk_path(index)):
            raise SystemExit(
                f"shard {chunk_path(index)} is missing; rerun with --resume")
        records.extend(load_chunk(index))

    checksum = hashlib.sha256(
        json.dumps(records, sort_keys=True).encode()).hexdigest()[:16]

    # Colour and phase balance is recorded in the metadata so that every later
    # experiment can assert on it without re-parsing the file, and printed so a
    # skewed pool is visible in the generation log rather than at match time.
    colour = Counter()
    colour_phase = Counter()
    for record in records:
        board = chess.Board(record["fen"])
        stm = "white" if board.turn else "black"
        colour[stm] += 1
        colour_phase[(stm, record["phase"])] += 1
    total = max(len(records), 1)
    balance = {
        "white": colour["white"],
        "black": colour["black"],
        "white_pct": round(100.0 * colour["white"] / total, 2),
        "by_phase": {phase: {"white": colour_phase[("white", phase)],
                             "black": colour_phase[("black", phase)]}
                     for phase in ("opening_middlegame", "late_middlegame", "endgame")},
    }
    print(f"colour balance: white {colour['white']} "
          f"({balance['white_pct']:.1f}%) black {colour['black']}", flush=True)
    for phase, counts in balance["by_phase"].items():
        phase_total = counts["white"] + counts["black"]
        share = 100.0 * counts["white"] / max(phase_total, 1)
        print(f"  {phase:20s} white={counts['white']:7d} black={counts['black']:7d} "
              f"white_pct={share:5.1f}%", flush=True)
    if abs(balance["white_pct"] - 50.0) > 5.0:
        print(f"WARNING: colour balance is {balance['white_pct']:.1f}% white, "
              "outside +/-5 of parity. The dataset is imbalanced; do not use it "
              "for strength conclusions until the cause is understood.", flush=True)

    metadata = {
        "format": FORMAT,
        "positions": len(records),
        "dropped": len_unique - len(records),
        "harvested": harvest_count,
        "unique_harvested": len_unique,
        "depth": args.depth,
        "shallow_depth": args.shallow_depth,
        "selfplay_games": args.selfplay_games,
        "selfplay_depth": args.selfplay_depth,
        "random_games": args.random_games,
        "harvest_every": args.harvest_every,
        "harvest_every_late": args.harvest_every_late,
        "late_from_ply": args.late_from_ply,
        "min_ply": args.min_ply,
        "max_plies": args.max_plies,
        "opening_min": args.opening_min,
        "opening_max": args.opening_max,
        "seed": args.seed,
        "workers": args.workers,
        "harvest_every_odd": True,
        "colour_balance": balance,
        "engine": ENGINE,
        "generator": "nnue/generate_v3.py",
        "generator_version": GENERATOR_VERSION,
        "generator_commit": commit,
        "checksum": checksum,
        "label_seconds": round(time.time() - t1, 1),
        "resumed_chunks": resumed_chunks,
    }

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    write_json(args.output, {"metadata": metadata, "records": records}, indent=2)

    print(f"wrote {len(records)} records ({len_unique - len(records)} dropped) "
          f"to {args.output}", flush=True)
    print(f"checksum {checksum}", flush=True)
    print(f"total {time.time()-t0:.0f}s", flush=True)
    if args.clean_shards:
        shutil.rmtree(shards_dir, ignore_errors=True)
        print(f"removed {shards_dir}", flush=True)


if __name__ == "__main__":
    main()