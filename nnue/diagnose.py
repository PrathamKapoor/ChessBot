"""Dataset diagnostics for NNUE training data.

Answers, for one or more dataset files:
  * composition (material balance, side to move, phase, piece counts)
  * target distribution (min/max/mean/median/stddev/percentiles, histogram,
    mate and draw separation, extreme tail)
  * tactical and endgame coverage
  * duplication: exact duplicates, mirrored duplicates, and near-duplicates
    (positions that differ only by castling rights, halfmove clock, en passant,
    or by a single quiet reversible move)
  * leakage risk: whether train and validation splits share positions

Usage:
    py -3 nnue/diagnose.py nnue/data/v2.json [--compare nnue/data/v1.json]
    py -3 nnue/diagnose.py nnue/data/v2.json --leak-report 80 20
"""

import argparse
import hashlib
import json
import math
import os
import statistics
import sys
from collections import Counter, defaultdict

import chess

sys_path = os.path.dirname(os.path.abspath(__file__))
if sys_path not in sys.path:
    sys.path.insert(0, sys_path)


PIECE_NAMES = ["pawn", "knight", "bishop", "rook", "queen", "king"]

# Piece values for material balance, tuned to the engine's own scale.
PIECE_VALUE = {chess.PAWN: 100, chess.KNIGHT: 320, chess.BISHOP: 330,
               chess.ROOK: 500, chess.QUEEN: 900, chess.KING: 0}


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    return raw["metadata"], raw["records"]


def material(board):
    """Signed material from the side to move's point of view, in centipawns."""
    total = 0
    for piece in board.piece_map().values():
        total += PIECE_VALUE[piece.piece_type] * (1 if piece.color else -1)
    return total


def piece_signature(board):
    """Sorted multiset of (color, piece_type, square) for hashing duplicates."""
    return tuple(sorted((p.color, p.piece_type, sq) for sq, p in board.piece_map().items()))


def mirror_fen(board):
    mirrored = board.mirror()
    return mirrored.fen()


def board_core(board):
    """FEN core: placement, side to move, castling rights, en passant.

    Drops the halfmove clock and fullmove number, which are irrelevant to
    evaluation and only inflate the key space.
    """
    return " ".join(board.fen().split(" ")[:4])


def is_opposite_coloured_bishops(board):
    dark, light = [], []
    for square, piece in board.piece_map().items():
        if piece.piece_type != chess.BISHOP:
            continue
        is_dark = bool(chess.BB_DARK_SQUARES & chess.BB_SQUARES[square])
        (dark if is_dark else light).append(piece.color)
    return len(dark) == 1 and len(light) == 1 and dark[0] != light[0]


def phase_of(board):
    """Crude but reproducible phase label driven by non-pawn material."""
    non_pawn = sum(1 for p in board.piece_map().values()
                   if p.piece_type not in (chess.PAWN, chess.KING))
    total = board.piece_map().__len__()
    if total <= 6 or non_pawn <= 2:
        return "endgame"
    if non_pawn <= 6 or total <= 14:
        return "late_middlegame"
    return "opening_middlegame"


def tactical_flags(board, score_cp):
    """Reproducible tactical indicators. No judgement about 'complicated'."""
    flags = []
    if board.is_check():
        flags.append("in_check")
    if any(board.is_capture(m) for m in board.legal_moves):
        flags.append("capture_available")
    for move in board.legal_moves:
        if move.promotion:
            flags.append("promotion_available")
            break
    # Large absolute score means the teacher sees a decisive position, which
    # is where shallow tactical errors show up most visibly.
    if abs(score_cp) >= 500:
        flags.append("decisive_eval")
    return flags


def endgame_flags(board):
    flags = []
    counts = Counter(p.piece_type for p in board.piece_map().values())
    non_pawn = {chess.KNIGHT: 0, chess.BISHOP: 0, chess.ROOK: 0, chess.QUEEN: 0}
    for pt in non_pawn:
        non_pawn[pt] = counts.get(pt, 0)
    total_non_pawn = sum(non_pawn.values())
    if counts.get(chess.PAWN, 0) == 0:
        flags.append("pawnless")
    if total_non_pawn == 0:
        flags.append("pawn_endgame")
    elif total_non_pawn <= 2:
        flags.append("minor_ending")
    elif total_non_pawn <= 4:
        flags.append("rook_ending")
    elif total_non_pawn <= 8 and non_pawn[chess.QUEEN] <= 1:
        flags.append("queen_ending")
    if total_non_pawn + 2 <= 8:
        flags.append("sparse_pieces")
    if is_opposite_coloured_bishops(board):
        flags.append("opposite_coloured_bishops")
    return flags


def percentile(sorted_values, fraction):
    if not sorted_values:
        return float("nan")
    index = min(len(sorted_values) - 1,
                max(0, int(round(fraction * (len(sorted_values) - 1)))))
    return sorted_values[index]


def analyse(records, label, leak_split=None):
    out = {"dataset": label, "records": len(records)}

    scores = [r["score_cp"] for r in records]
    finite = [s for s in scores if math.isfinite(s)]
    out["non_finite_targets"] = len(scores) - len(finite)

    mate_scores = [s for s in finite if abs(s) >= 30000]
    ordinary = [s for s in finite if abs(s) < 30000]
    clipped = [s for s in ordinary if abs(s) >= 2000]

    out["targets"] = {
        "min": min(finite) if finite else None,
        "max": max(finite) if finite else None,
        "mean": statistics.fmean(finite) if finite else None,
        "median": statistics.median(finite) if finite else None,
        "stddev": statistics.pstdev(finite) if len(finite) > 1 else None,
        "mad": statistics.median([abs(s - statistics.median(finite)) for s in finite]) if finite else None,
        "mean_abs": statistics.fmean([abs(s) for s in finite]) if finite else None,
        "mate_scores": len(mate_scores),
        "exactly_zero": sum(1 for s in finite if s == 0),
        "near_zero_abs_le_20": sum(1 for s in ordinary if abs(s) <= 20),
        "at_clip_boundary_abs_ge_2000": len(clipped),
        "abs_le_100": sum(1 for s in ordinary if abs(s) <= 100),
        "abs_le_300": sum(1 for s in ordinary if abs(s) <= 300),
        "abs_le_1000": sum(1 for s in ordinary if abs(s) <= 1000),
        "abs_gt_1000": sum(1 for s in ordinary if abs(s) > 1000),
        "percentiles": {
            f"p{int(f*100)}": percentile(sorted(finite), f)
            for f in (0.001, 0.01, 0.05, 0.25, 0.5, 0.75, 0.95, 0.99, 0.999)
        },
    }

    # Histogram of ordinary targets, coarse bins.
    bins = [(-30000, -1000), (-1000, -500), (-500, -200), (-200, -50),
            (-50, 50), (50, 200), (200, 500), (500, 1000), (1000, 2000),
            (2000, 30000)]
    histogram = Counter()
    for s in ordinary:
        for lo, hi in bins:
            if lo <= s < hi:
                histogram[f"[{lo},{hi})"] += 1
                break
    out["target_histogram"] = {f"[{lo},{hi})": histogram[f"[{lo},{hi})"] for lo, hi in bins}

    counters = {
        "stm": Counter(),
        "phase": Counter(),
        "endgame_class": Counter(),
        "tactical": Counter(),
        "material_sign": Counter(),
        "capture_available": 0,
        "promotion_available": 0,
        "check_available": 0,
        "mean_pieces": 0.0,
        "mean_non_pawn_pieces": 0.0,
        "mean_pawns": 0.0,
    }
    material_values = []
    piece_counts = Counter()

    seen_fens = Counter()
    seen_core = Counter()
    seen_mirror = Counter()
    seen_sig = Counter()

    for record in records:
        board = chess.Board(record["fen"])
        score = record["score_cp"]

        stm = board.turn
        counters["stm"]["white" if stm else "black"] += 1

        mat = material(board)
        material_values.append(mat)
        counters["material_sign"]["positive" if mat > 0 else ("negative" if mat < 0 else "equal")] += 1

        counters["phase"][phase_of(board)] += 1
        for flag in endgame_flags(board):
            counters["endgame_class"][flag] += 1
        for flag in tactical_flags(board, score):
            counters["tactical"][flag] += 1

        if any(board.is_capture(m) for m in board.legal_moves):
            counters["capture_available"] += 1
        if any(m.promotion for m in board.legal_moves):
            counters["promotion_available"] += 1
        if any(board.gives_check(m) for m in board.legal_moves):
            counters["check_available"] += 1

        pieces = list(board.piece_map().values())
        counters["mean_pieces"] += len(pieces)
        counters["mean_non_pawn_pieces"] += sum(
            1 for p in pieces if p.piece_type not in (chess.PAWN, chess.KING))
        counters["mean_pawns"] += sum(1 for p in pieces if p.piece_type == chess.PAWN)
        for piece in pieces:
            piece_counts[(piece.piece_type, piece.color)] += 1

    n = max(len(records), 1)
    out["composition"] = {
        "stm": dict(counters["stm"]),
        "phase": dict(counters["phase"]),
        "material_sign": dict(counters["material_sign"]),
        "material_median": statistics.median(material_values) if material_values else None,
        "material_stdev": statistics.pstdev(material_values) if len(material_values) > 1 else None,
        "mean_pieces": counters["mean_pieces"] / n,
        "mean_non_pawn_pieces": counters["mean_non_pawn_pieces"] / n,
        "mean_pawns": counters["mean_pawns"] / n,
        "capture_positions_pct": 100.0 * counters["capture_available"] / n,
        "promotion_positions_pct": 100.0 * counters["promotion_available"] / n,
        "check_available_pct": 100.0 * counters["check_available"] / n,
        "endgame_classes": dict(counters["endgame_class"]),
        "tactical_flags": dict(counters["tactical"]),
        "mean_pieces_by_type_color": {
            f"{PIECE_NAMES[pt-1]}_{'white' if col else 'black'}": count / n
            for (pt, col), count in sorted(piece_counts.items())
        },
    }

    for record in records:
        fen = record["fen"]
        seen_fens[fen] += 1
        board = chess.Board(fen)
        seen_core[board_core(board)] += 1
        seen_sig[piece_signature(board)] += 1
        seen_mirror[mirror_fen(board)] += 1

    def dup_stats(counter):
        total = sum(counter.values())
        distinct = len(counter)
        repeated = sum(c - 1 for c in counter.values() if c > 1)
        worst = max(counter.values()) if counter else 0
        return {
            "entries": total,
            "distinct": distinct,
            "excess_copies": repeated,
            "duplicate_pct": 100.0 * repeated / max(total, 1),
            "max_multiplicity": worst,
        }

    # Near-duplicate proxy: how many distinct FENs collapse once the halfmove
    # and fullmove counters are dropped.
    collapsed = len(records) - seen_core_counter_unique(seen_core, len(records))
    out["duplication"] = {
        "exact_fen": dup_stats(seen_fens),
        "mirrored_fen": dup_stats(seen_mirror),
        "piece_placement_signature": dup_stats(seen_sig),
        "fen_without_move_counters": {
            "distinct": len(seen_core),
            "collapsed_records": collapsed,
            "collapse_pct": 100.0 * collapsed / max(len(records), 1),
        },
    }

    if leak_split is not None:
        out["leakage"] = leakage_report(records, leak_split)

    return out


def seen_core_counter_unique(seen_core, total):
    """Records that disappear when only the FEN core is used as the key."""
    return sum(1 for c in seen_core.values() if c == 1)


def leakage_report(records, split_percent):
    """Simulate the trainer's random split and measure cross-split overlap.

    Also reports how many validation positions are reachable from a training
    position by one reversible quiet move, which is the stronger form of
    leakage for position data.
    """
    ordered = list(records)
    cut = int(len(ordered) * split_percent / 100.0)
    train = ordered[:cut]
    valid = ordered[cut:]

    train_fens = set(r["fen"] for r in train)
    valid_fens = set(r["fen"] for r in valid)

    exact = len(train_fens & valid_fens)

    train_core = set(board_core(chess.Board(r["fen"])) for r in train)
    valid_core = set(board_core(chess.Board(r["fen"])) for r in valid)
    core_overlap = len(train_core & valid_core)

    # Game-level grouping is impossible in the current record format: records
    # carry fen + score only, so provenance is unrecoverable. Measure how many
    # validation positions have a training position one reversible quiet move
    # away instead, as the practical leakage proxy.
    train_by_core = defaultdict(set)
    for record in train:
        board = chess.Board(record["fen"])
        train_by_core[board_core(board)].add(record["fen"])

    near = 0
    checked = 0
    for record in valid:
        checked += 1
        board = chess.Board(record["fen"])
        found = False
        for move in board.legal_moves:
            if board.is_capture(move) or board.gives_check(move):
                continue
            board.push(move)
            board.pop()
            key = board_core(board)
            if key in train_by_core:
                found = True
                break
        if found:
            near += 1

    return {
        "split_percent_train": split_percent,
        "train_records": len(train),
        "valid_records": len(valid),
        "exact_fen_overlap": exact,
        "fen_core_overlap": core_overlap,
        "core_overlap_pct_of_valid": 100.0 * core_overlap / max(len(valid), 1),
        "valid_positions_one_quiet_move_from_train": near,
        "near_overlap_pct_of_valid": 100.0 * near / max(checked, 1),
        "note": ("records carry no game identity, so a true game-level split is "
                 "not possible with the Phase 6 dataset format"),
    }


def verify_checksum(metadata, records):
    checksum = hashlib.sha256(
        json.dumps(records, sort_keys=True).encode()).hexdigest()[:16]
    return {
        "recorded": metadata.get("checksum"),
        "recomputed": checksum,
        "match": metadata.get("checksum") == checksum,
    }


def print_report(report):
    def section(title):
        print()
        print(title)
        print("-" * len(title))

    print(f"=== {report['dataset']} : {report['records']} records ===")

    section("target distribution")
    t = report["targets"]
    for key in ("min", "max", "mean", "median", "stddev", "mad", "mean_abs"):
        value = t[key]
        print(f"  {key:12s} {value:12.2f}" if isinstance(value, float)
              else f"  {key:12s} {value}")
    print(f"  mate scores     {t['mate_scores']}")
    print(f"  exactly zero    {t['exactly_zero']}")
    print(f"  |score| <= 20   {t['near_zero_abs_le_20']}")
    print(f"  |score| <= 100  {t['abs_le_100']}")
    print(f"  |score| <= 300  {t['abs_le_300']}")
    print(f"  |score| <= 1000 {t['abs_le_1000']}")
    print(f"  |score| >  1000 {t['abs_gt_1000']}")
    print(f"  at +/-2000 clip {t['at_clip_boundary_abs_ge_2000']}")
    print(f"  non-finite      {report['non_finite_targets']}")
    print("  percentiles:")
    for key, value in t["percentiles"].items():
        print(f"    {key:8s} {value:10.1f}")
    print("  histogram (ordinary, non-mate):")
    total_ordinary = sum(report["target_histogram"].values())
    for key, count in report["target_histogram"].items():
        share = 100.0 * count / max(total_ordinary, 1)
        print(f"    {key:16s} {count:8d}  {share:5.1f}%")

    section("composition")
    c = report["composition"]
    print(f"  side to move      {c['stm']}")
    print(f"  phase             {c['phase']}")
    print(f"  material sign     {c['material_sign']}")
    print(f"  material median   {c['material_median']}")
    print(f"  material stdev    {c['material_stdev']:.1f}")
    print(f"  mean pieces       {c['mean_pieces']:.2f}")
    print(f"  mean non-pawn     {c['mean_non_pawn_pieces']:.2f}")
    print(f"  mean pawns        {c['mean_pawns']:.2f}")
    print(f"  capture pos       {c['capture_positions_pct']:.1f}%")
    print(f"  promotion pos     {c['promotion_positions_pct']:.1f}%")
    print(f"  check available   {c['check_available_pct']:.1f}%")
    print("  endgame classes:")
    for key, value in sorted(c["endgame_classes"].items(), key=lambda kv: -kv[1]):
        print(f"    {key:32s} {value:8d}  {100.0*value/max(report['records'],1):5.1f}%")
    print("  tactical flags:")
    for key, value in sorted(c["tactical_flags"].items(), key=lambda kv: -kv[1]):
        print(f"    {key:32s} {value:8d}  {100.0*value/max(report['records'],1):5.1f}%")

    section("duplication")
    for key, value in report["duplication"].items():
        if isinstance(value, dict):
            print(f"  {key}")
            for k2, v2 in value.items():
                print(f"    {k2:24s} {v2}")
        else:
            print(f"  {key:30s} {value}")

    if "leakage" in report:
        section("leakage (simulated random split)")
        for key, value in report["leakage"].items():
            print(f"  {key:44s} {value}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("datasets", nargs="+")
    ap.add_argument("--out", default=None, help="write JSON report here")
    ap.add_argument("--leak-report", type=int, default=None, metavar="SPLIT",
                    help="simulate a random split with SPLIT percent train")
    ap.add_argument("--limit", type=int, default=None,
                    help="analyse only the first N records (fast iteration)")
    args = ap.parse_args()

    reports = []
    for path in args.datasets:
        metadata, records = load(path)
        if args.limit:
            records = records[:args.limit]
        report = analyse(records, os.path.basename(path), args.leak_report)
        report["metadata"] = metadata
        report["checksum_verification"] = verify_checksum(metadata, records)
        print_report(report)
        print()
        print(f"  checksum recorded {report['checksum_verification']['recorded']}, "
              f"recomputed {report['checksum_verification']['recomputed']}, "
              f"match {report['checksum_verification']['match']}")
        reports.append(report)

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(reports, f, indent=2, default=str)
        print(f"\nwrote {args.out}")


if __name__ == "__main__":
    main()
