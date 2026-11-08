# PocketChess Companion

A Chrome or Edge extension with its own chessboard. Stockfish 19 is the default playing engine; the original PocketChess engine is also available in the Playing engine menu. The extension does not access or control Chess.com. Enter the computer opponent's moves on the companion board and copy the engine's moves to the computer game.

## Setup

1. Install Python 3.10+ and the dependency: `python -m pip install python-chess`.
2. For Stockfish, download the official Windows x86-64 universal release from https://github.com/official-stockfish/Stockfish/releases/tag/sf_19 and extract it so the executable is at `third_party\stockfish\stockfish\stockfish-windows-x86-64-universal.exe`. Stockfish is separately licensed under GPLv3; the release includes its license and source. The downloaded files are not tracked in this repository. To use the original PocketChess engine instead, build it with `powershell -ExecutionPolicy Bypass -File .\build.ps1` if `build\pocketchess.exe` is not present.
3. In Chrome open `chrome://extensions` (in Edge use `edge://extensions`), enable **Developer mode**, click **Load unpacked**, and select the `extension` folder in this repository.
4. In PowerShell at the repository root run `python tools\companion_bridge.py`. Leave this window open.
5. Click the PocketChess Companion extension icon to open its board. Select the engine's color and click **New game**. Make the engine's shown move in your computer-opponent game, then enter the opponent's reply by clicking its piece and destination in the companion.

Use **Undo turn** to correct an incorrectly entered reply, and **Reconnect / retry** if the bridge was restarted. Changing color starts a new game. The game is saved locally in the browser. The bridge listens only on 127.0.0.1:8765 and accepts requests from browser-extension origins; other installed extensions can potentially access it. Stop it with Ctrl+C.

Use the companion against computer opponents only. Chess.com prohibits engine assistance in human games. The extension neither injects into nor reads Chess.com pages.


## Board controls

- Click an opponent piece to see its legal destinations, then click the destination.
- Right-click a square to highlight it; right-drag between squares to draw or remove an arrow. Use **Clear arrows and highlights** to remove all annotations.
- The checked king is highlighted in red. Checkmate and the winner are shown above the final move; copy that move to the computer game.
- Toggle **Sound effects** in the controls. Browser audio may require a click on the page first.
