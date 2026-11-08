import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from companion_bridge import position_info


class PositionTests(unittest.TestCase):
    def test_replays_capture_and_tracks_turn(self):
        info = position_info(['d2d4', 'g8f6', 'b1c3', 'f6d5', 'c3d5'])
        self.assertEqual(info['turn'], 'black')
        self.assertEqual(info['pieces']['d5'], 'N')
        self.assertNotIn('c3', info['pieces'])

    def test_rejects_bad_move_instead_of_silently_truncating(self):
        with self.assertRaises(ValueError):
            position_info(['d2d4', 'xd5'])

    def test_start_position_lists_legal_moves(self):
        self.assertIn('e2e4', position_info([])['legal'])


class GameStatusTests(unittest.TestCase):
    def test_checkmate_reports_winner_and_checked_king(self):
        info = position_info(['f2f3', 'e7e5', 'g2g4', 'd8h4'])
        self.assertEqual(info['termination'], 'checkmate')
        self.assertEqual(info['winner'], 'black')
        self.assertEqual(info['checked_king'], 'e1')

    def test_check_is_reported_without_game_over(self):
        info = position_info(['e2e4', 'f7f6', 'd1h5'])
        self.assertEqual(info['checked_king'], 'e8')
        self.assertIsNone(info['termination'])


class EngineSelectionTests(unittest.TestCase):
    def test_stockfish_is_default_and_returns_engine_identity(self):
        from companion_bridge import ENGINES, handle
        if not ENGINES['stockfish'].is_file():
            self.skipTest('Stockfish is not installed')
        result = handle({'action': 'think', 'moves': [], 'movetime': 100})
        self.assertIn('Stockfish', result['engine_name'])
        self.assertIn(result['bestmove'], result['legal'])

    def test_rejects_unknown_engine(self):
        from companion_bridge import handle
        with self.assertRaises(ValueError):
            handle({'action': 'think', 'engine': 'unknown', 'moves': []})


class OpeningDiversityTests(unittest.TestCase):
    def test_white_first_moves_are_diverse(self):
        from companion_bridge import handle
        moves = set()
        for _ in range(30):
            res = handle({'action': 'think', 'moves': [], 'book': True})
            moves.add(res['bestmove'])
        # Should have multiple distinct opening moves for White (e.g. e4, d4, c4, Nf3)
        self.assertGreater(len(moves), 1)

    def test_black_replies_to_e4_are_diverse(self):
        from companion_bridge import handle
        moves = set()
        for _ in range(30):
            res = handle({'action': 'think', 'moves': ['e2e4'], 'book': True})
            moves.add(res['bestmove'])
        # Should not only be e5; should also produce Sicilian, French, Caro-Kann, etc.
        self.assertGreater(len(moves), 1)

    def test_black_replies_to_d4_are_not_only_indian(self):
        from companion_bridge import handle
        moves = set()
        for _ in range(30):
            res = handle({'action': 'think', 'moves': ['d2d4'], 'book': True})
            moves.add(res['bestmove'])
        # Should produce both Classical Queen's Pawn (d5) and Indian (Nf6)
        self.assertIn('d7d5', moves)
        self.assertIn('g8f6', moves)

    def test_disabling_book_uses_engine(self):
        from companion_bridge import handle
        res = handle({'action': 'think', 'moves': [], 'book': False, 'engine': 'pocketchess', 'movetime': 100})
        self.assertIn('PocketChess', res['engine_name'])
        self.assertNotIn('(Book:', res['engine_name'])

    def test_kings_gambit_is_available(self):
        from companion_bridge import handle
        moves = set()
        for _ in range(60):
            res = handle({'action': 'think', 'moves': ['e2e4', 'e7e5'], 'book': True})
            moves.add(res['bestmove'])
        self.assertIn('f2f4', moves)  # King's Gambit

    def test_spanish_and_scotch_are_available(self):
        from companion_bridge import handle
        moves = set()
        for _ in range(60):
            res = handle({'action': 'think', 'moves': ['e2e4', 'e7e5', 'g1f3', 'b8c6'], 'book': True})
            moves.add(res['bestmove'])
        self.assertIn('f1b5', moves)  # Spanish Game / Ruy Lopez
        self.assertIn('d2d4', moves)  # Scotch Game

    def test_petrov_is_available(self):
        from companion_bridge import handle
        moves = set()
        for _ in range(60):
            res = handle({'action': 'think', 'moves': ['e2e4', 'e7e5', 'g1f3'], 'book': True})
            moves.add(res['bestmove'])
        self.assertIn('g8f6', moves)  # Petrov's Defence

    def test_french_and_queens_openings_are_available(self):
        from companion_bridge import handle
        # French defence response to e4
        e4_replies = set()
        for _ in range(60):
            res = handle({'action': 'think', 'moves': ['e2e4'], 'book': True})
            e4_replies.add(res['bestmove'])
        self.assertIn('e7e6', e4_replies)  # French Defence

        # Queen's Gambit response to d4 d5
        d4_replies = set()
        for _ in range(60):
            res = handle({'action': 'think', 'moves': ['d2d4', 'd7d5'], 'book': True})
            d4_replies.add(res['bestmove'])
        self.assertIn('c2c4', d4_replies)  # Queen's Gambit

    def test_indian_game_branches_are_available(self):
        from companion_bridge import handle
        moves = set()
        for _ in range(60):
            res = handle({'action': 'think', 'moves': ['d2d4', 'g8f6', 'c2c4'], 'book': True})
            moves.add(res['bestmove'])
        self.assertIn('g7g6', moves)  # King's Indian / Grünfeld
        self.assertIn('e7e6', moves)  # Nimzo-Indian / Queen's Indian / Bogo
        self.assertIn('c7c5', moves)  # Modern Benoni / Benko


if __name__ == '__main__':
    unittest.main()
