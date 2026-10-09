"""Dataset integrity, checksum and split-leakage tests (Phase 7 gate).

Part 27 of the Phase 7 plan lists dataset integrity tests, dataset checksum
verification and train/validation leakage checks as mandatory gates. These
run against whatever is currently in nnue/data, so they are the gate for
every dataset version created in this phase, not a one-off script run.

    py -3 -m unittest discover -s nnue -p "test_*.py" -v
    py -3 nnue/test_datasets.py            # same via unittest.main

Covered:

  checksum      recompute the record checksum and compare with the metadata
  structure     record count matches metadata, required fields are present,
                scores are finite, game ids exist on format v3 pools
  duplication   no exact duplicate FENs (full scan for pools up to 150k
                records, seeded 20k sample above that)
  splits        the game-level split has zero game overlap between train and
                validation; the record-level split has zero exact FEN overlap
  sampling      the policies repeat exactly the records the flags say they
                should, and the weight histogram matches
"""

import json
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from train import apply_sampling_policy, bucket_of, split_records  # noqa: E402

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
FULL_SCAN_LIMIT = 150000
SAMPLE_SIZE = 20000


def checksum(records):
    import hashlib
    return hashlib.sha256(
        json.dumps(records, sort_keys=True).encode()).hexdigest()[:16]


def dataset_paths():
    if not os.path.isdir(DATA_DIR):
        return []
    return sorted(os.path.join(DATA_DIR, f)
                  for f in os.listdir(DATA_DIR) if f.endswith(".json"))


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


class TestDatasetIntegrity(unittest.TestCase):
    def test_datasets_exist(self):
        self.assertTrue(dataset_paths(),
                        "no datasets found in nnue/data; expected at least "
                        "the v1/v2 pools or a generated pool")

    def test_checksums_and_structure(self):
        for path in dataset_paths():
            raw = load(path)
            meta, records = raw["metadata"], raw["records"]
            name = os.path.basename(path)
            with self.subTest(dataset=name):
                expected = meta.get("subset_records", meta.get("positions"))
                self.assertEqual(len(records), expected,
                                 f"{name}: record count does not match metadata")
                expected_checksum = (meta.get("subset_checksum")
                                     if "subset_records" in meta
                                     else meta["checksum"])
                self.assertEqual(checksum(records), expected_checksum,
                                 f"{name}: checksum mismatch (file altered?)")
                self.assertGreater(len(records), 0)
                for record in records:
                    self.assertIn("fen", record)
                    self.assertIn("score_cp", record)
                    score = record["score_cp"]
                    self.assertTrue(-30000 <= score <= 30000,
                                    f"{name}: score {score} out of range")
                    if meta.get("format", 0) >= 3:
                        self.assertIn("game_id", record)

    def test_no_duplicate_fens(self):
        for path in dataset_paths():
            raw = load(path)
            records = raw["records"]
            name = os.path.basename(path)
            if len(records) > FULL_SCAN_LIMIT:
                sample = random.Random(1234).sample(records, SAMPLE_SIZE)
            else:
                sample = records
            with self.subTest(dataset=name):
                fens = [r["fen"] for r in sample]
                self.assertEqual(len(fens), len(set(fens)),
                                 f"{name}: duplicate FENs in sample of {len(fens)}")

    def test_colour_balance_within_tolerance(self):
        """A pool more than 5% off parity is a generator defect (16e7cd1).

        Datasets from generator version 3 are exempt: the v3 pool is the
        recorded instance of that defect, kept as a negative-result artifact,
        and its colour balance is documented in RESEARCH.md rather than
        asserted here. Generator version 4 onward refuses even harvest strides
        and records the balance, so it must pass.
        """
        for path in dataset_paths():
            raw = load(path)
            records = raw["records"]
            meta = raw["metadata"]
            name = os.path.basename(path)
            if len(records) < 5000:
                continue  # smoke datasets are too small to mean anything
            if meta.get("generator_version", 0) == 3:
                continue
            sample = records if len(records) <= 100000 else \
                random.Random(7).sample(records, 100000)
            import chess
            white = sum(1 for r in sample[:20000] if chess.Board(r["fen"]).turn)
            share = 100.0 * white / min(len(sample), 20000)
            with self.subTest(dataset=name):
                self.assertLess(abs(share - 50.0), 5.0,
                                f"{name}: {share:.1f}% white to move")


class TestSplits(unittest.TestCase):
    def setUp(self):
        self.records = []
        rng = random.Random(99)
        for game in range(40):
            for ply in range(12):
                self.records.append({
                    "fen": f"fen-{game}-{ply}", "game_id": f"g{game:03d}",
                    "score_cp": rng.randint(-500, 500), "ply": ply,
                    "phase": "opening_middlegame", "flags": [], "endgame": [],
                })

    def test_game_split_has_no_game_overlap(self):
        train, valid = split_records(self.records, "game", 0.8, 42)
        self.assertTrue(train and valid)
        train_games = {r["game_id"] for r in train}
        valid_games = {r["game_id"] for r in valid}
        self.assertEqual(train_games & valid_games, set())
        self.assertEqual(len(train_games) + len(valid_games), 40)

    def test_record_split_has_no_fen_overlap(self):
        train, valid = split_records(self.records, "record", 0.8, 42)
        self.assertEqual({r["fen"] for r in train} & {r["fen"] for r in valid},
                         set())

    def test_game_split_is_deterministic(self):
        a = split_records(self.records, "game", 0.8, 42)
        b = split_records(list(self.records), "game", 0.8, 42)
        self.assertEqual([r["fen"] for r in a[0]], [r["fen"] for r in b[0]])
        self.assertEqual([r["fen"] for r in a[1]], [r["fen"] for r in b[1]])


class TestSamplingPolicies(unittest.TestCase):
    def setUp(self):
        self.records = [
            {"fen": "a", "game_id": "g", "score_cp": 10, "ply": 1,
             "phase": "opening_middlegame", "flags": [], "endgame": [],
             "shallow_cp": 10},
            {"fen": "b", "game_id": "g", "score_cp": -800, "ply": 2,
             "phase": "endgame", "flags": ["in_check"], "endgame": ["minor_ending"],
             "shallow_cp": -100},
            {"fen": "c", "game_id": "g", "score_cp": 0, "ply": 3,
             "phase": "late_middlegame", "flags": ["capture_available"],
             "endgame": ["rook_ending"], "shallow_cp": 300},
            {"fen": "d", "game_id": "g", "score_cp": 30, "ply": 4,
             "phase": "late_middlegame", "flags": [],
             "endgame": ["queen_ending", "sparse_pieces"], "shallow_cp": 30},
        ]

    def test_baseline_is_noop(self):
        out, info = apply_sampling_policy(self.records, "baseline", 3, 42)
        self.assertEqual([r["fen"] for r in out], ["a", "b", "c", "d"])
        self.assertEqual(info["boost"], 1)

    def test_tactical_repeats_flagged_and_volatile(self):
        out, info = apply_sampling_policy(self.records, "tactical", 3, 42)
        counts = {fen: sum(1 for r in out if r["fen"] == fen)
                  for fen in "abcd"}
        # b: in_check; c: capture alone is NOT tactical, but it is volatile
        # (300 vs 0 is >= 200); d: queen_ending is not a tactical flag
        self.assertEqual(counts, {"a": 1, "b": 3, "c": 3, "d": 1})
        self.assertEqual(info["weight_histogram"], {1: 2, 3: 2})

    def test_endgame_repeats_strict_classes_only(self):
        out, info = apply_sampling_policy(self.records, "endgame", 3, 42)
        counts = {fen: sum(1 for r in out if r["fen"] == fen)
                  for fen in "abcd"}
        # b: endgame phase + minor_ending; c: rook_ending; d: only broad
        # classes (queen_ending, sparse_pieces) and a late-middlegame phase
        self.assertEqual(counts, {"a": 1, "b": 3, "c": 3, "d": 1})

    def test_bucket_priority(self):
        # tactical beats endgame (b is in check and in an endgame phase)
        self.assertEqual(bucket_of(self.records[1]), "tactical")
        # a record with only a strict endgame class is endgame
        self.assertEqual(bucket_of({"score_cp": 50, "flags": [],
                                    "endgame": ["rook_ending"],
                                    "phase": "late_middlegame"}), "endgame")
        # broad classes alone (queen_ending is non_pawn<=8 with <=1 queen, which
        # is most of the pool) do not make an endgame bucket
        self.assertEqual(bucket_of({"score_cp": 500, "flags": [],
                                    "endgame": ["queen_ending"],
                                    "phase": "late_middlegame"}), "ordinary")
        self.assertEqual(bucket_of({"score_cp": 1500, "flags": [], "endgame": [],
                                    "phase": "opening_middlegame"}), "extreme")
        self.assertEqual(bucket_of({"score_cp": 20, "flags": [], "endgame": [],
                                    "phase": "opening_middlegame"}), "quiet")


if __name__ == "__main__":
    unittest.main(verbosity=2)
