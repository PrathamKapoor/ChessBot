# PocketChess / ChessBot

[![Build & Tests](https://img.shields.io/badge/tests-passing-brightgreen.svg)]()
[![Standard](https://img.shields.io/badge/C%2B%2B-20-blue.svg)]()
[![Hardware Acceleration](https://img.shields.io/badge/SIMD-AVX2%20%2B%20BMI2%20(PEXT)-orange.svg)]()
[![Protocol](https://img.shields.io/badge/protocol-UCI-purple.svg)]()

A high-performance, independently developed, tournament-ready C++20 UCI chess engine combining hardware-accelerated bitboard move generation, efficient Principal Variation Search (PVS), and dual classical/embedded NNUE evaluation.

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
  - **Vectorized Embedded NNUE:** HalfKP feature transformer evaluated via AVX2 SIMD intrinsics with int16 accumulators and clipped activations, trained on Huber loss with bounded clipping.

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
|  - Embedded HalfKP AVX2 NNUE (nnue/evaluation_bridge.cpp)            |
+----------------------------------------------------------------------+
```

---

## Building from Source

### Prerequisites
- C++20 compliant compiler supporting AVX2 and BMI2 (e.g. GCC 13+, Clang 16+, MSYS2 UCRT64).
- PowerShell (on Windows) or Bash (on Linux).
- Python 3.10+ (for training and match testing).

### Compile
Run the build script from the project root:
```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Target all
```
Or build only the test binaries:
```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Target tests
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
- `UseNnue` (check, default false): Toggle between NNUE and Classical PeSTO evaluation.
- `NnuePath` (string): Path to export binary NNUE network file (`.bin`).

Compatible with standard chess GUIs: CuteChess, Arena, Banksia GUI, Python-Chess, and Fritz.

---

## Engineering Documentation

- [`decisions.md`](file:///C:/Projects/ChessBot/decisions.md): Detailed architecture decisions, rationale, trade-offs, and empirical findings.
- [`flow.md`](file:///C:/Projects/ChessBot/flow.md): Complete control flow, data transformations, and algorithm lifecycles.
- [`handoff.md`](file:///C:/Projects/ChessBot/handoff.md): Current engineering state, operational manual, and validation badges.
- [`RESEARCH.md`](file:///C:/Projects/ChessBot/RESEARCH.md): Historical research findings, benchmarking logs, and experiment notes.
