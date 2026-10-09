"""Relabel a sample of the pool's records with a fresh teacher and compare.

Gate for the labelling pipeline: search is deterministic at fixed depth with a
cleared TT, so a fresh engine must reproduce both the primary and the shallow
label exactly. If it does not, something in the labelling path (TT carry-over,
ucinewgame handling, score-sign handling) is wrong and every dataset built this
way is suspect.
"""

import json
import random
import sys

import chess
import chess.engine

path = sys.argv[1] if len(sys.argv) > 1 else "nnue/data/v4_pool.json.shards/chunk_0000.json"
n = int(sys.argv[2]) if len(sys.argv) > 2 else 20

raw = json.load(open(path, encoding="utf-8"))
records = raw["records"] if isinstance(raw, dict) and "records" in raw else raw
sample = random.Random(4).sample(records, min(n, len(records)))

engine = chess.engine.SimpleEngine.popen_uci("build/pocketchess.exe")
mismatch = 0
checked = 0
try:
    for record in sample:
        board = chess.Board(record["fen"])
        values = {}
        for depth in (record["depth"], record.get("shallow_depth")):
            if not depth:
                continue
            engine.protocol.send_line("ucinewgame")
            result = engine.play(board, chess.engine.Limit(depth=depth),
                                 info=chess.engine.INFO_ALL)
            score = result.info.get("score")
            if score is None or score.relative is None:
                values[depth] = None
                continue
            rel = score.relative
            values[depth] = (30000.0 if rel.mate() > 0 else -30000.0) \
                if rel.is_mate() else float(rel.score())
        checked += 1
        primary_ok = values[record["depth"]] == record["score_cp"]
        shallow_ok = ("shallow_cp" not in record or
                      values.get(record.get("shallow_depth")) == record["shallow_cp"])
        if not (primary_ok and shallow_ok):
            mismatch += 1
            print(f"MISMATCH {record['fen']}  stored d{record['depth']}="
                  f"{record['score_cp']} fresh={values[record['depth']]} "
                  f"shallow stored={record.get('shallow_cp')} "
                  f"fresh={values.get(record.get('shallow_depth'))}")
finally:
    engine.close()

print(f"checked {checked} records, mismatches {mismatch}")
print("LABEL DETERMINISM OK" if mismatch == 0 else "LABEL DETERMINISM FAILED")
sys.exit(0 if mismatch == 0 else 1)
