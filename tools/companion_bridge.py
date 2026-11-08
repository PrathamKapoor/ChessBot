"""Local-only bridge between the extension's board and the PocketChess UCI engine."""
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import chess
import chess.engine
from opening_book import get_book_move

ROOT = Path(__file__).resolve().parents[1]
ENGINES = {
    'stockfish': ROOT / 'third_party' / 'stockfish' / 'stockfish' / 'stockfish-windows-x86-64-universal.exe',
    'pocketchess': ROOT / 'build' / 'pocketchess.exe',
}


def make_board(moves):
    if not isinstance(moves, list) or len(moves) > 1000:
        raise ValueError('Invalid move history')
    board = chess.Board()
    for move in moves:
        try:
            board.push_uci(move)
        except (ValueError, TypeError) as exc:
            raise ValueError(f'Invalid or illegal move: {move}') from exc
    return board


def position_info(moves):
    board = make_board(moves)
    outcome = board.outcome(claim_draw=True)
    return {'pieces': {chess.square_name(s): p.symbol() for s, p in board.piece_map().items()},
            'turn': 'white' if board.turn else 'black',
            'legal': [m.uci() for m in board.legal_moves],
            'outcome': outcome.result() if outcome else None,
            'termination': outcome.termination.name.lower() if outcome else None,
            'winner': ('white' if outcome.winner else 'black') if outcome and outcome.winner is not None else None,
            'checked_king': chess.square_name(board.king(board.turn)) if board.is_check() else None}


def handle(data):
    moves = data.get('moves', [])
    board = make_board(moves)
    info = position_info(moves)
    if data.get('action') == 'think' and not info['outcome']:
        engine_key = data.get('engine', 'stockfish')
        if engine_key not in ENGINES:
            raise ValueError('Unknown engine selection')
        engine_path = ENGINES[engine_key]
        if not engine_path.is_file():
            raise ValueError(f'{engine_key} executable not found: {engine_path}. See extension/README.md for setup.')
        milliseconds = int(data.get('movetime', 1000))
        if not 100 <= milliseconds <= 30000:
            raise ValueError('Thinking time must be between 100 and 30000 ms')

        # Check opening book first if requested (defaults to True for opening diversity)
        use_book = data.get('book', True)
        if use_book:
            book_result = get_book_move(board)
            if book_result is not None:
                move, san, opening_name = book_result
                info['bestmove'] = move.uci()
                info['san'] = san
                info['opening'] = opening_name
                engine_display = 'Stockfish 19' if engine_key == 'stockfish' else 'PocketChess'
                info['engine_name'] = f"{engine_display} (Book: {opening_name})"
                return info

        with chess.engine.SimpleEngine.popen_uci(str(engine_path), timeout=40) as engine:
            if engine_key == 'stockfish':
                engine.configure({'Threads': min(4, os.cpu_count() or 1), 'Hash': 64})
            result = engine.play(board, chess.engine.Limit(time=milliseconds / 1000))
            info['bestmove'] = result.move.uci()
            info['san'] = board.san(result.move)
            info['engine_name'] = engine.id.get('name', engine_key)
    return info


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        origin = self.headers.get('Origin', '')
        if not origin.startswith(('chrome-extension://', 'moz-extension://')):
            self.send_error(403, 'Only browser extensions can access this bridge')
            return
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= 32768:
                raise ValueError('Invalid request size')
            data = json.loads(self.rfile.read(length))
            response = handle(data)
            code = 200
        except (ValueError, TypeError, KeyError, json.JSONDecodeError) as exc:
            response, code = {'error': str(exc)}, 400
        except (OSError, chess.engine.EngineError, TimeoutError) as exc:
            response, code = {'error': f'Engine error: {exc}'}, 500
        body = json.dumps(response).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', origin)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        origin = self.headers.get('Origin', '')
        if not origin.startswith(('chrome-extension://', 'moz-extension://')):
            self.send_error(403)
            return
        self.send_response(204)
        self.send_header('Access-Control-Allow-Origin', origin)
        self.send_header('Access-Control-Allow-Methods', 'POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()


if __name__ == '__main__':
    print('PocketChess bridge running at http://127.0.0.1:8765. Keep this window open.', flush=True)
    ThreadingHTTPServer(('127.0.0.1', 8765), Handler).serve_forever()
