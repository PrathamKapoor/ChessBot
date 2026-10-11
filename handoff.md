# Engineering Handoff: Current State & Operational Manual

Authoritative engineering handover documentation for ChessBot / PocketChess.

---

## 1. Executive Summary & Verification Badges

- **System Name:** PocketChess / ChessBot
- **Author Identity:** `PrathamKapoor` (`prathamkapoor027@gmail.com`)
- **Primary Toolchain:** C++20 (MSYS2 UCRT64 GCC 15.1.0), Python 3.13.14, AVX2 + BMI2 (PEXT).
- **Core Architecture:** 64-bit Bitboard Engine, Hardware PEXT Slider Lookups, Negamax PVS Search with Aspiration Windows, Reverse Futility Pruning, Razoring, Check Extensions, Generational Transposition Table, PeSTO Tapered Classical Eval, AVX2-Vectorized HalfKP Embedded NNUE.
- **Verification State:**
  - `bitboard_test`: **VERIFIED PASS** (262,144 ray comparisons, 4,096 square pairs).
  - `geometry_test`: **VERIFIED PASS**.
  - `eval_test`: **VERIFIED PASS** (152 checks).
  - `tt_test`: **VERIFIED PASS** (5,147 adversarial and collision checks).
  - `search_test`: **VERIFIED PASS** (325 checks, including TT persistence across moves).
  - `tactical_test`: **VERIFIED PASS** (17 of 17 tactical positions solved).
  - `golden_test.py`: **VERIFIED PASS** (24/24 golden vectors, 0.00e+00 delta).
  - `match/selftest.py`: **VERIFIED PASS** (31/31 unit tests passed).
  - `test_datasets.py`: **VERIFIED PASS** (12/12 pytest cases passed).

---

## 2. Directory Structure and Component Mapping

```text
C:\Projects\ChessBot\
├── engine/                       # Core C++20 Engine Sources
│   ├── bitboard.hpp / .cpp       # 64-bit bitboard operations & ray tables
│   ├── geometry.hpp / .cpp       # Square coordinates, distances & alignment
│   ├── movegen.hpp / .cpp        # Legal move generation & BMI2 PEXT slider lookups
│   ├── position.hpp / .cpp       # Board representation, state stack, Zobrist hashing
│   ├── evaluate.hpp / .cpp       # PeSTO tapered classical evaluation
│   ├── search.hpp / .cpp         # Negamax PVS search, quiescence, time management
│   ├── tt.hpp / .cpp             # 16-byte Transposition Table with generational aging
│   ├── uci.hpp / .cpp            # UCI protocol parser and game driver
│   ├── types.hpp                 # Fundamental chess types, square enums, piece defs
│   └── main.cpp                  # Engine entrypoint binary
├── nnue/                         # Neural Network Training & Inference
│   ├── evaluation_bridge.hpp     # AVX2 C++ inference bridge and global NNUE state
│   ├── evaluation_bridge.cpp     # SIMD feature transformer accumulator & linear eval
│   ├── train.py                  # PyTorch Huber loss training script
│   ├── export.py                 # Binary network exporter (.pt -> .bin)
│   ├── golden_test.py            # C++ vs Python floating-point parity gate
│   ├── dataset.py                # Dataset parsing and streaming loaders
│   └── test_datasets.py          # Pytest suite for training datasets
├── match/                        # Automated Competitive & Regression Match System
│   ├── matchrunner.py            # Head-to-head match runner with clock management
│   ├── matchlib.py               # Engine process spawning, I/O piping, UCI protocols
│   ├── sprt.py                   # Sequential Probability Ratio Testing (SPRT)
│   └── selftest.py               # Complete test harness for match infrastructure
├── tests/                        # Comprehensive C++ Test Suites
│   ├── bitboard_test.cpp         # Bitboard primitive verification
│   ├── geometry_test.cpp         # Board geometry unit tests
│   ├── eval_test.cpp             # Evaluation symmetry and score bounds
│   ├── search_test.cpp           # Search correctness, PVS invariants, UCI replay, TT persistence
│   ├── tactical_test.cpp         # 17-puzzle tactical regression suite
│   └── tt_test.cpp               # Transposition table collision & aging tests
├── tools/                        # Benchmarks and Profilers
│   ├── bench.cpp                 # Movegen and perft benchmarking
│   ├── search_bench.cpp          # Search NPS and node count benchmark
│   └── eval_microbench.cpp       # Nanosecond evaluation profiling
├── build.ps1                     # Unified PowerShell compilation script
├── decisions.md                  # Mandatory architecture decision log (WHY)
├── flow.md                       # Mandatory system architecture and flow doc (HOW)
├── handoff.md                    # Mandatory current state & operations manual (THIS FILE)
└── README.md                     # Public documentation and hackathon showcase
```

---

## 3. Build, Execution, and Testing Instructions

### 3.1 Compiling All Binaries
Run from PowerShell in the repository root:
```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Target all
```
Or build only the test suite:
```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Target tests
```
Artifacts are compiled with optimization `-O3 -mavx2 -mbmi -mbmi2 -mpopcnt -funroll-loops` into `build/`.

### 3.2 Running the Full Test Suite
Execute the verified C++ test executables:
```powershell
.\build\bitboard_test.exe
.\build\geometry_test.exe
.\build\eval_test.exe
.\build\tt_test.exe
.\build\search_test.exe
.\build\tactical_test.exe
```
Execute the Python verification tests:
```powershell
python -m pytest nnue/test_datasets.py
python nnue/golden_test.py
python match/selftest.py
```

### 3.3 Interactive UCI Run
Launch the engine:
```powershell
.\build\pocketchess.exe
```
Example UCI commands:
```text
uci
isready
ucinewgame
position startpos moves e2e4 e7e5 g1f3 b8c6
go depth 7
quit
```

---

## 4. Known Findings and Optimization Notes

1. **Transposition Table Persistence:**
   - Previous revisions cleared the table on each search. The engine now increments table generation (`newSearch()`) without zeroing entries, preserving valuable transpositions across moves while allowing new searches to overwrite stale entries.
   - Verified via `testTTPersistenceAcrossMoves` in `search_test.cpp`.
2. **Move Generation Performance:**
   - PEXT sliding piece generation delivers over 95M NPS on Zen 3 hardware, with legal move generation taking less than 1.2k cycles per position.
3. **NNUE Clipping Strategy:**
   - Research revealed that discarding extreme scores (`noxtreme`) produced a net that failed to convert won positions (-41.9 Elo). Bounded clipping (`clip1000`) and Huber loss stabilize gradients without dropping the critical conversion tail.

---

## 5. Hackathon Demonstration Checklist

- [x] **Verified Standalone Executable:** `build/pocketchess.exe` compiled and self-contained.
- [x] **Standard UCI Compliance:** Compatible with CuteChess, Arena, Python-Chess, and Lichess bots.
- [x] **Reproducible Benchmarks:** `build/search_bench.exe` and `build/perft.exe` verify NPS and speed.
- [x] **Tactical Puzzles Pass:** 17/17 tactical puzzle benchmarks pass within fractions of a second.
- [x] **Clean Documentation:** Architecture (`decisions.md`, `flow.md`, `handoff.md`, `README.md`) clearly explained without marketing hyperbole.
