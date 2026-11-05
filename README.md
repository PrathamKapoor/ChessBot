# PocketChess / ChessBot

[![Standard](https://img.shields.io/badge/C%2B%2B-20-blue.svg)]()
[![Hardware Acceleration](https://img.shields.io/badge/SIMD-AVX2%20%2B%20BMI2%20(PEXT)-orange.svg)]()
[![Protocol](https://img.shields.io/badge/protocol-UCI-purple.svg)]()

A C++20 UCI chess engine combining bitboard move generation, Principal Variation Search (PVS), and classical evaluation, with experimental NNUE training and evaluation tools.

---

## Key Features & Technical Highlights

- **Pure Bitboard Core & Hardware PEXT Sliders:**
  - 64-bit bitboard board representation with state stack rewind.
  - Zero-multiplication sliding piece ray lookups powered by x86-64 BMI2 `_pext_u64` (Parallel Bit Extract) instructions.
  - Measured core move generation speed exceeding **95 Million Nodes/Second** on AMD Zen 3 hardware.

- **Principled, Efficient Search Engine:**
  - **Negamax Alpha-Beta with Principal Variation Search (PVS):** Full window on candidate moves, narrow null-window searches on rest.
  - **Aspiration Windows:** Tight search bounds with fail-low/fail-high recovery reducing search trees by 20-30%.
  - **Selective Pruning:** Reverse Futility Pruning (RFP) and Razoring with zugzwang and check defenses.
  - **Check Extensions:** Deep tactical horizon preservation when king is in check.
  - **Move Ordering Pipeline:** Strict disjoint priority bands combining Transposition Table hits, Static Exchange Evaluation (SEE) with MVV-LVA, Killer heuristic, and history tables.

- **Persistent Transposition Table:**
  - 16-byte packed entries containing full 64-bit Zobrist keys (zero false-match collision risk).
  - Generational aging (`newSearch()`) preserving deep transpositions across moves while allowing fresher nodes to replace stale branches.

- **Dual-Evaluation Architecture:**
  - **Classical Tapered PeSTO Evaluation:** Zero-dependency positional evaluation with game-phase interpolated piece-square tables, pawn structure analysis, open-file rook bonuses, and king safety penalties.
  - **Experimental NNUE:** HalfKP features, AVX2 inference, and scripts for training and evaluating networks. Networks and datasets are not included in the repository.

- **Rigorous Verification & Tooling:**
  - Automated differential oracle tests comparing against reference Negamax.
  - 100% pass rate on 17-puzzle tactical regression suite and bitboard ray invariant tests.
  - Decoupled tournament match runner with automated clock enforcement and Wald SPRT statistical hypothesis testing.

---

## Architecture Overview

```
                      +-----------------------------+
                      |         UCI Front-End       |
                      |      (engine/uci.cpp)       |
                      +--------------+--------------+
                                     |
              +----------------------+----------------------+
              |                                             |
   [Position Commands]                                [Go Commands]
              |                                             |
   +----------v-----------+                      +----------v----------+
   |   Position & State   |                      |     Alpha-Beta      |
   | (engine/position.*)  |                      |     Search Loop     |
   +----------+-----------+                      | (engine/search.*)   |
              |                                  +----------+----------+
              |                                             |
+-------------v--------------+                   +----------v----------+
|  Move Generation & PEXT    |                   |  Transposition      |
|    (engine/movegen.*)      |                   |  Table (Aging)      |
+----------------------------+                   |  (engine/tt.*)      |
              |                                  +----------+----------+
              |                                             |
+-------------v---------------------------------------------v----------+
|                         Evaluation Pipeline                          |
|  - Classical PeSTO Tapered Eval (engine/evaluate.cpp)                |
|  - Experimental HalfKP AVX2 NNUE (nnue/evaluation_bridge.hpp)        |
+----------------------------------------------------------------------+
```

---

## Building from Source

### Prerequisites
- C++20 compliant compiler supporting AVX2 and BMI2 (e.g. GCC 13+, Clang 16+, MSYS2 UCRT64).
- PowerShell and a compatible `g++` on PATH; the supplied build script targets Windows.
- Python 3.10+ (for training and match testing).
- Python tools use `python-chess`; NNUE training and golden tests also require PyTorch. Install `pytest` to run the Python tests. For example: `python -m pip install python-chess torch pytest`.

The engine itself requires no API keys, environment file, database, backend, or frontend. The build uses x86 AVX2 and BMI2 instructions; it requires a compatible CPU. Training data and network checkpoints are excluded from Git and must be generated separately for NNUE experiments.

### Compile
Run the build script from the project root:
```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```
For a debug build:
```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config debug
```
The compiled binaries will be output to `build/`:
- `build/pocketchess.exe` (Main UCI engine)
- `build/search_bench.exe` (Search performance benchmark)
- `build/perft.exe` (Perft movegen speed test)
- `build/*_test.exe` (Test suite binaries)

---

## Verification & Test Suite

Run the full automated test suite to verify engine correctness and invariants:

```powershell
# C++ Invariant and Regression Tests
.\build\bitboard_test.exe     # Bitboard primitives and ray tables
.\build\geometry_test.exe     # Square geometry and ray relations
.\build\eval_test.exe         # Evaluation score symmetry
.\build\tt_test.exe           # Transposition table replacement & aging
.\build\search_test.exe       # Alpha-beta search & TT persistence
.\build\tactical_test.exe     # 17 tactical puzzles (tactical accuracy)

# Python Match and Training Invariant Tests
python -m pytest nnue/test_datasets.py
python nnue/golden_test.py
python match/selftest.py
```

---

## Running the Engine (UCI Protocol)

Launch the standalone executable:
```powershell
.\build\pocketchess.exe
```

Interact directly via the Universal Chess Interface (UCI):
```text
uci
isready
ucinewgame
position startpos moves e2e4 e7e5 g1f3 b8c6
go depth 6
```

Supported UCI options:
- `Hash` (spin, default 16, min 1, max 1024): Transposition table size in megabytes.
- `Threads` (spin, fixed at 1) and `Ponder` are advertised for GUI compatibility.

The engine also accepts `NnuePath` and `UseNnue` through `setoption`; NNUE is experimental and requires a separately generated network.

Compatible with standard chess GUIs: CuteChess, Arena, Banksia GUI, Python-Chess, and Fritz.

---

## Engineering Documentation

- [`decisions.md`](decisions.md): Architecture decisions and trade-offs.
- [`flow.md`](flow.md): Control flow and algorithm lifecycles.
- [`handoff.md`](handoff.md): Engineering state and operational notes.
- [`RESEARCH.md`](RESEARCH.md): Research findings, benchmarks, and experiments.
