"""Curated opening book for PocketChess and companion bridge.

Provides diverse, standard master-level opening lines to prevent repetitive play,
spanning King's Gambit, Spanish Game (Ruy Lopez), Scotch Game & Gambit, Petrov's Defence,
French Opening, Queen's Openings (QGD, QGA, Slav, Semi-Slav, London, Colle, Torre),
Indian Games (King's Indian, Nimzo-Indian, Queen's Indian, Grünfeld, Bogo-Indian, Benoni, Benko),
Sicilian variations (Najdorf, Dragon, Sveshnikov, Classical, Alapin, Smith-Morra),
Caro-Kann variations, and Flank Openings (English, Réti, Bird, KIA).
"""
import random
import chess

# (name, weight, move_sequence)
# Lines are verified 100% legal UCI coordinates.
OPENING_REPERTOIRE = [
    # --- King's Gambit & Open Gambits ---
    ("King's Gambit Accepted (Cunningham)", 20, [
        "e2e4", "e7e5", "f2f4", "e5f4", "g1f3", "f8e7", "f1c4", "e7h4"
    ]),
    ("King's Gambit Accepted (Classical)", 20, [
        "e2e4", "e7e5", "f2f4", "e5f4", "g1f3", "g7g5", "h2h4", "g5g4", "f3e5", "g8f6", "d2d4", "d7d6", "e5d3", "f6e4"
    ]),
    ("King's Gambit Declined", 15, [
        "e2e4", "e7e5", "f2f4", "f8c5", "g1f3", "d7d6", "b1c3", "g8f6", "f1c4", "b8c6"
    ]),
    ("Falkbeer Counter-Gambit", 15, [
        "e2e4", "e7e5", "f2f4", "d7d5", "e4d5", "e5e4", "d2d3", "g8f6", "d3e4", "f6e4"
    ]),
    ("Evans Gambit", 20, [
        "e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5", "b2b4", "c5b4", "c2c3", "b4a5", "d2d4", "e5d4", "e1g1"
    ]),
    ("Danish Gambit", 10, [
        "e2e4", "e7e5", "d2d4", "e5d4", "c2c3", "d4c3", "f1c4", "c3b2", "c1b2"
    ]),
    ("Vienna Game & Gambit", 15, [
        "e2e4", "e7e5", "b1c3", "g8f6", "f2f4", "d7d5", "f4e5", "f6e4", "g1f3", "f8e7", "d2d4", "e8g8"
    ]),

    # --- Spanish Game (Ruy Lopez) ---
    ("Spanish Game (Closed Morphy)", 30, [
        "e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5a4", "g8f6", "e1g1", "f8e7", "f1e1", "b7b5", "a4b3", "d7d6", "c2c3", "e8g8"
    ]),
    ("Spanish Game (Berlin Defence)", 25, [
        "e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "g8f6", "e1g1", "f6e4", "d2d4", "e4d6", "b5c6", "d7c6", "d4e5", "d6f5"
    ]),
    ("Spanish Game (Exchange)", 15, [
        "e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5c6", "d7c6", "e1g1", "f7f6", "d2d4", "e5d4", "f3d4", "c6c5"
    ]),
    ("Spanish Game (Open)", 15, [
        "e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5a4", "g8f6", "e1g1", "f6e4", "d2d4", "b7b5", "a4b3", "d7d5", "d4e5", "c8e6"
    ]),
    ("Spanish Game (Steinitz)", 15, [
        "e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5a4", "d7d6", "c2c3", "c8d7", "d2d4", "g8f6"
    ]),

    # --- Scotch Game & Gambits ---
    ("Scotch Game (Mieses)", 25, [
        "e2e4", "e7e5", "g1f3", "b8c6", "d2d4", "e5d4", "f3d4", "g8f6", "d4c6", "b7c6", "e4e5", "d8e7", "d1e2", "f6d5"
    ]),
    ("Scotch Game (Classical)", 20, [
        "e2e4", "e7e5", "g1f3", "b8c6", "d2d4", "e5d4", "f3d4", "f8c5", "c1e3", "d8f6", "c2c3", "g8e7", "f1c4"
    ]),
    ("Scotch Gambit", 20, [
        "e2e4", "e7e5", "g1f3", "b8c6", "d2d4", "e5d4", "f1c4", "f8c5", "c2c3", "g8f6", "e4e5", "d7d5", "c4b5", "f6e4"
    ]),
    ("Scotch Four Knights", 15, [
        "e2e4", "e7e5", "g1f3", "b8c6", "d2d4", "e5d4", "f3d4", "g8f6", "b1c3", "f8b4", "d4c6", "b7c6", "f1d3", "d7d5"
    ]),

    # --- Petrov's Defence ---
    ("Petrov's Defence (Classical)", 25, [
        "e2e4", "e7e5", "g1f3", "g8f6", "f3e5", "d7d6", "e5f3", "f6e4", "d2d4", "d6d5", "f1d3", "f8e7", "e1g1", "b8c6", "c2c4"
    ]),
    ("Petrov's Defence (Steinitz)", 20, [
        "e2e4", "e7e5", "g1f3", "g8f6", "d2d4", "f6e4", "f1d3", "d7d5", "f3e5", "b8d7", "e1g1", "d7e5", "d4e5"
    ]),
    ("Petrov's Defence (Three Knights)", 15, [
        "e2e4", "e7e5", "g1f3", "g8f6", "b1c3", "f8b4", "f3e5", "e8g8", "f1e2", "f8e8", "e5d3", "b4c3", "d2c3", "f6e4"
    ]),

    # --- Italian Game ---
    ("Italian Game (Giuoco Piano)", 30, [
        "e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5", "c2c3", "g8f6", "d2d3", "d7d6"
    ]),
    ("Italian Game (Two Knights)", 25, [
        "e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "g8f6", "d2d3", "f8c5", "c2c3", "e8g8"
    ]),

    # --- French Opening ---
    ("French Defence (Winawer)", 25, [
        "e2e4", "e7e6", "d2d4", "d7d5", "b1c3", "f8b4", "e4e5", "c7c5", "a2a3", "b4c3", "b2c3", "g8e7", "d1g4"
    ]),
    ("French Defence (Classical)", 20, [
        "e2e4", "e7e6", "d2d4", "d7d5", "b1c3", "g8f6", "c1g5", "f8e7", "e4e5", "f6d7", "h2h4", "a7a6"
    ]),
    ("French Defence (Rubinstein)", 15, [
        "e2e4", "e7e6", "d2d4", "d7d5", "b1c3", "d5e4", "c3e4", "b8d7", "g1f3", "g8f6", "e4f6", "d7f6"
    ]),
    ("French Defence (Tarrasch)", 20, [
        "e2e4", "e7e6", "d2d4", "d7d5", "b1d2", "c7c5", "e4d5", "e6d5", "g1f3", "b8c6", "f1b5", "f8d6"
    ]),
    ("French Defence (Advance)", 20, [
        "e2e4", "e7e6", "d2d4", "d7d5", "e4e5", "c7c5", "c2c3", "b8c6", "g1f3", "d8b6", "a2a3"
    ]),
    ("French Defence (Exchange)", 15, [
        "e2e4", "e7e6", "d2d4", "d7d5", "e4d5", "e6d5", "g1f3", "g8f6", "f1d3", "f8d6", "e1g1", "e8g8"
    ]),
    ("French Defence (King's Indian Attack)", 15, [
        "e2e4", "e7e6", "d2d3", "d7d5", "b1d2", "g8f6", "g1f3", "c7c5", "g2g3", "b8c6", "f1g2", "f8e7", "e1g1", "e8g8"
    ]),

    # --- Queen's Opening / Queen's Pawn Games ---
    ("Queen's Gambit Declined (Tartakower)", 30, [
        "d2d4", "d7d5", "c2c4", "e7e6", "b1c3", "g8f6", "c1g5", "f8e7", "e2e3", "e8g8", "g1f3", "h7h6", "g5h4", "b7b6"
    ]),
    ("Queen's Gambit Accepted", 20, [
        "d2d4", "d7d5", "c2c4", "d5c4", "g1f3", "g8f6", "e2e3", "e7e6", "f1c4", "c7c5", "e1g1", "a7a6"
    ]),
    ("Slav Defence", 30, [
        "d2d4", "d7d5", "c2c4", "c7c6", "g1f3", "g8f6", "b1c3", "d5c4", "a2a4", "c8f5", "e2e3", "e7e6", "f1c4", "f8b4"
    ]),
    ("Semi-Slav Defence (Meran)", 25, [
        "d2d4", "d7d5", "c2c4", "c7c6", "g1f3", "g8f6", "b1c3", "e7e6", "e2e3", "b8d7", "f1d3", "d5c4", "d3c4", "b7b5", "c4d3"
    ]),
    ("London System", 30, [
        "d2d4", "d7d5", "c1f4", "g8f6", "e2e3", "c7c5", "c2c3", "b8c6", "b1d2", "d8b6", "d1b3", "c5c4"
    ]),
    ("Torre Attack", 15, [
        "d2d4", "d7d5", "g1f3", "g8f6", "c1g5", "e7e6", "e2e3", "f8e7", "b1d2", "b8d7", "f1d3", "c7c5", "c2c3"
    ]),
    ("Colle System", 15, [
        "d2d4", "d7d5", "g1f3", "g8f6", "e2e3", "e7e6", "f1d3", "c7c5", "c2c3", "b8c6", "b1d2", "f8d6"
    ]),
    ("Albin Counter-Gambit", 10, [
        "d2d4", "d7d5", "c2c4", "e7e5", "d4e5", "d5d4", "g1f3", "b8c6", "a2a3", "c8g4"
    ]),
    ("Chigorin Defence", 10, [
        "d2d4", "d7d5", "c2c4", "b8c6", "g1f3", "c8g4", "c4d5", "g4f3", "g2f3", "d8d5", "e2e3", "e7e5", "b1c3", "f8b4"
    ]),

    # --- Indian Games ---
    ("King's Indian Defence (Classical)", 30, [
        "d2d4", "g8f6", "c2c4", "g7g6", "b1c3", "f8g7", "e2e4", "d7d6", "g1f3", "e8g8", "f1e2", "e7e5", "e1g1", "b8c6", "d4d5", "c6e7"
    ]),
    ("King's Indian Defence (Sämisch)", 20, [
        "d2d4", "g8f6", "c2c4", "g7g6", "b1c3", "f8g7", "e2e4", "d7d6", "f2f3", "e8g8", "c1e3", "e7e5", "d4d5", "c7c6"
    ]),
    ("Nimzo-Indian Defence (Rubinstein)", 30, [
        "d2d4", "g8f6", "c2c4", "e7e6", "b1c3", "f8b4", "e2e3", "e8g8", "f1d3", "d7d5", "g1f3", "c7c5", "e1g1", "b8c6"
    ]),
    ("Nimzo-Indian Defence (Classical)", 20, [
        "d2d4", "g8f6", "c2c4", "e7e6", "b1c3", "f8b4", "d1c2", "e8g8", "a2a3", "b4c3", "c2c3", "b7b6", "c1g5"
    ]),
    ("Queen's Indian Defence", 20, [
        "d2d4", "g8f6", "c2c4", "e7e6", "g1f3", "b7b6", "g2g3", "c8a6", "b2b3", "f8b4", "c1d2", "b4e7", "f1g2", "c7c6"
    ]),
    ("Grünfeld Defence (Exchange)", 25, [
        "d2d4", "g8f6", "c2c4", "g7g6", "b1c3", "d7d5", "c4d5", "f6d5", "e2e4", "d5c3", "b2c3", "f8g7", "g1f3", "c7c5"
    ]),
    ("Grünfeld Defence (Russian)", 20, [
        "d2d4", "g8f6", "c2c4", "g7g6", "b1c3", "d7d5", "g1f3", "f8g7", "d1b3", "d5c4", "b3c4", "e8g8", "e2e4", "c7c6"
    ]),
    ("Bogo-Indian Defence", 15, [
        "d2d4", "g8f6", "c2c4", "e7e6", "g1f3", "f8b4", "c1d2", "d8e7", "g2g3", "b8c6", "f1g2", "b4d2", "b1d2"
    ]),
    ("Modern Benoni", 15, [
        "d2d4", "g8f6", "c2c4", "c7c5", "d4d5", "e7e6", "b1c3", "e6d5", "c4d5", "d7d6", "e2e4", "g7g6", "g1f3", "f8g7"
    ]),
    ("Benko Gambit", 15, [
        "d2d4", "g8f6", "c2c4", "c7c5", "d4d5", "b7b5", "c4b5", "a7a6", "b5a6", "g7g6", "b1c3", "c8a6"
    ]),
    ("Budapest Gambit", 15, [
        "d2d4", "g8f6", "c2c4", "e7e5", "d4e5", "f6g4", "c1f4", "b8c6", "g1f3", "f8b4", "b1d2", "d8e7"
    ]),
    ("Old Indian Defence", 10, [
        "d2d4", "g8f6", "c2c4", "d7d6", "b1c3", "e7e5", "g1f3", "b8d7", "e2e4", "f8e7"
    ]),
    ("Catalan Opening", 20, [
        "d2d4", "g8f6", "c2c4", "e7e6", "g2g3", "d7d5", "f1g2", "f8e7", "g1f3", "e8g8", "e1g1", "d5c4", "d1c2"
    ]),

    # --- Sicilian Defence Variations ---
    ("Sicilian Defence (Najdorf)", 35, [
        "e2e4", "c7c5", "g1f3", "d7d6", "d2d4", "c5d4", "f3d4", "g8f6", "b1c3", "a7a6", "c1e3", "e7e5"
    ]),
    ("Sicilian Defence (Dragon)", 25, [
        "e2e4", "c7c5", "g1f3", "d7d6", "d2d4", "c5d4", "f3d4", "g8f6", "b1c3", "g7g6", "c1e3", "f8g7"
    ]),
    ("Sicilian Defence (Sveshnikov)", 20, [
        "e2e4", "c7c5", "g1f3", "b8c6", "d2d4", "c5d4", "f3d4", "g8f6", "b1c3", "e7e5", "d4b5", "d7d6", "c1g5", "a7a6", "b5a3", "b7b5"
    ]),
    ("Sicilian Defence (Classical)", 20, [
        "e2e4", "c7c5", "g1f3", "b8c6", "d2d4", "c5d4", "f3d4", "g8f6", "b1c3", "d7d6"
    ]),
    ("Sicilian Defence (Kan / Taimanov)", 20, [
        "e2e4", "c7c5", "g1f3", "e7e6", "d2d4", "c5d4", "f3d4", "b8c6", "b1c3", "a7a6"
    ]),
    ("Sicilian Defence (Alapin)", 15, [
        "e2e4", "c7c5", "c2c3", "d7d5", "e4d5", "d8d5", "d2d4", "g8f6", "g1f3", "e7e6"
    ]),
    ("Sicilian Defence (Closed)", 15, [
        "e2e4", "c7c5", "b1c3", "b8c6", "g2g3", "g7g6", "f1g2", "f8g7", "d2d3", "d7d6"
    ]),
    ("Smith-Morra Gambit", 15, [
        "e2e4", "c7c5", "d2d4", "c5d4", "c2c3", "d4c3", "b1c3", "b8c6", "g1f3", "d7d6", "f1c4", "e7e6"
    ]),

    # --- Caro-Kann Variations ---
    ("Caro-Kann Defence (Classical)", 25, [
        "e2e4", "c7c6", "d2d4", "d7d5", "b1c3", "d5e4", "c3e4", "c8f5", "e4g3", "f5g6"
    ]),
    ("Caro-Kann Defence (Advance)", 25, [
        "e2e4", "c7c6", "d2d4", "d7d5", "e4e5", "c8f5", "g1f3", "e7e6", "f1e2", "c6c5"
    ]),
    ("Caro-Kann Defence (Panov-Botvinnik)", 15, [
        "e2e4", "c7c6", "d2d4", "d7d5", "e4d5", "c6d5", "c2c4", "g8f6", "b1c3", "e7e6", "g1f3", "f8e7"
    ]),

    # --- Other Defences & Flank Openings ---
    ("Scandinavian Defence", 20, [
        "e2e4", "d7d5", "e4d5", "d8d5", "b1c3", "d5a5", "d2d4", "g8f6", "g1f3", "c8f5"
    ]),
    ("Alekhine's Defence", 15, [
        "e2e4", "g8f6", "e4e5", "f6d5", "d2d4", "d7d6", "g1f3", "g7g6", "f1e2", "f8g7"
    ]),
    ("Pirc / Modern Defence", 15, [
        "e2e4", "d7d6", "d2d4", "g8f6", "b1c3", "g7g6", "g1f3", "f8g7", "f1e2", "e8g8"
    ]),
    ("Dutch Defence (Leningrad)", 20, [
        "d2d4", "f7f5", "c2c4", "g8f6", "g2g3", "g7g6", "f1g2", "f8g7", "g1f3", "e8g8", "e1g1", "d7d6"
    ]),
    ("English Opening (King's English)", 25, [
        "c2c4", "e7e5", "b1c3", "g8f6", "g1f3", "b8c6", "g2g3", "d7d5", "c4d5", "f6d5"
    ]),
    ("English Opening (Symmetrical)", 20, [
        "c2c4", "c7c5", "b1c3", "b8c6", "g1f3", "g8f6", "g2g3", "g7g6", "f1g2", "f8g7"
    ]),
    ("English Opening (Anglo-Indian)", 15, [
        "c2c4", "g8f6", "b1c3", "e7e6", "g1f3", "d7d5", "d2d4", "f8e7"
    ]),
    ("Réti Opening", 25, [
        "g1f3", "d7d5", "c2c4", "e7e6", "g2g3", "g8f6", "f1g2", "f8e7", "e1g1", "e8g8"
    ]),
    ("King's Indian Attack", 15, [
        "g1f3", "g8f6", "g2g3", "d7d5", "f1g2", "c7c6", "e1g1", "c8f5", "d2d3", "e7e6"
    ]),
]

class OpeningBook:
    """Compiled in-memory opening book indexed by position EPD."""

    def __init__(self):
        # epd -> dict(move_uci -> {'weight': int, 'name': str})
        self._table = {}
        self._compile()

    def _compile(self):
        for name, weight, line in OPENING_REPERTOIRE:
            board = chess.Board()
            for move_str in line:
                epd = board.epd()
                move = chess.Move.from_uci(move_str)
                if move not in board.legal_moves:
                    break
                entry = self._table.setdefault(epd, {})
                if move_str in entry:
                    entry[move_str]['weight'] += weight
                else:
                    entry[move_str] = {'weight': weight, 'name': name}
                board.push(move)

    def get_move(self, board: chess.Board):
        """Returns (chess.Move, san, opening_name) or None if out of book."""
        epd = board.epd()
        candidates = self._table.get(epd)
        if not candidates:
            return None

        # Filter to strictly legal moves
        legal_candidates = []
        weights = []
        for move_str, data in candidates.items():
            try:
                move = chess.Move.from_uci(move_str)
                if move in board.legal_moves:
                    legal_candidates.append((move, data['name']))
                    weights.append(data['weight'])
            except ValueError:
                continue

        if not legal_candidates:
            return None

        selected_move, name = random.choices(legal_candidates, weights=weights)[0]
        san = board.san(selected_move)
        return selected_move, san, name


# Singleton instance
_BOOK = OpeningBook()


def get_book_move(board: chess.Board):
    """Query the global opening book for the current board position."""
    return _BOOK.get_move(board)
