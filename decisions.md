# Architectural & Engineering Decisions

All major design decisions, trade-offs, rationale, and empirically verified findings across the development of PocketChess / ChessBot are recorded below.

---

## 1. Bitboard Representation and Move Generation

### 1.1 Pure Bitboard Architecture (Square-Centric vs Bitboard)
- **Decision:** Represent the board state using 64-bit unsigned integers (`uint64_t`) for piece locations and colors (`Position`), paired with state stacks (`StateInfo`) for reversible history.
- **Why:** Bitboards allow parallel operations over all squares using bitwise CPU instructions (`_mm_popcnt_u64`, `_tzcnt_u64`, `_pdep_u64`, `_pext_u64`). Square-centric board arrays require sequential scanning and branching.
- **Alternatives Considered:** 0x88 or 10x12 mailbox board representations.
- **Empirical Findings:** Measured perft throughput exceeds 95M NPS on AMD Ryzen 7 7435HS without discrete GPU assistance.

### 1.2 Hardware-Assisted PEXT Magic Sliders
- **Decision:** Utilize BMI2 `_pext_u64` (Parallel Bit Extract) instructions for rook and bishop slider attack generation rather than software magic bitboard multiplication or rotated bitboards.
- **Why:** Zen 3 / modern x86-64 processors implement `pext` in single-cycle micro-ops. PEXT eliminates table cache misses and large hash masks required by traditional magic bitboards.
- **Alternatives Considered:** Plain 32-bit/64-bit software magic bitboards, fancy magics.
- **Empirical Findings:** Sliding piece move generation dropped cycle overhead from 3,275 cycles/position to 1,193 cycles/position (2.7x speedup in core movegen).

### 1.3 State Tracking and Checkers Cache in `StateInfo`
- **Decision:** Cache `checkers_` bitboard and pin masks directly in `StateInfo`, restored during `undoMove()` rather than recomputed from scratch.
- **Why:** Computing checkers after `undoMove()` required redundant calls to `attackersTo()`.
- **Alternatives Considered:** Recomputing king attackers on every unmake.
- **Empirical Findings:** Accepting checkers caching produced a +6.4% best / +3.1% median NPS improvement across standard depth-5 test suites.

---

## 2. Evaluation System

### 2.1 Dual-Evaluation Paradigm: Classical Tapered PeSTO + NNUE HalfKP
- **Decision:** Maintain a verified, zero-dependency Classical evaluation engine alongside a vectorized embedded NNUE network with runtime switching (`UseNnue` UCI option).
- **Why:** The classical evaluation serves as a fast baseline, fallback oracle, and feature sanity check. Classical evaluation utilizes tapered game-phase interpolation (Opening/Endgame PeSTO tables, mobility, pawn structure, king safety).
- **Alternatives Considered:** Pure NNUE without classical fallback.
- **Trade-off:** Having both allows differential oracle testing where move ordering and search invariants are tested independently of neural network weights.

### 2.2 NNUE Architecture: HalfKP Feature Transformer with SIMD Dot Products
- **Decision:** Adopt `(40960 -> 256) x 2 -> 1` HalfKP feature transformer architecture with int16 accumulator quantization and int8 linear weights, evaluated via AVX2 vector intrinsics.
- **Why:** HalfKP (`king_square * 640 + piece_square`) balances expressive piece-square context with accumulator update efficiency. On mobile Zen 3 architecture, larger feature transformers (e.g. 1024x2 or dual networks) severely constrain search depth at fixed time controls.
- **Empirical Findings:** The SIMD accumulator evaluation achieves under 1.4 microseconds per full static evaluation, maintaining over 700k NPS under full alpha-beta tree search.

### 2.3 Training Loss: Huber Loss and Dynamic Clipping
- **Decision:** Train NNUE weights using Huber loss on centipawn-scaled evaluations rather than unconstrained MSE.
- **Why:** Deep mate scores and extreme tactical blunders create outsized gradients that destabilize feature transformer weights for quiet and positional play.
- **Empirical Findings (Case D in Research):** Completely dropping extreme scores (`noxtreme`) produced a net that fit quiet positions well (130.7 cp error) but performed worse in match play (-41.9 Elo) because it never learned conversion. Soft clipping (`clip1000` / `tanh500`) preserves tail gradients while bounding error amplification.

---

## 3. Search and Pruning Architecture

### 3.1 Negamax with Side-to-Move Convention
- **Decision:** Strictly enforce the Negamax convention where every evaluation and search score is relative to the side to move.
- **Why:** Eliminates White-relative coordinate conversions in recursion. Prevents color-inversion bugs.
- **Verification:** Unit tests (`testScoreConvention`) strictly verify sign symmetry across all piece configurations.

### 3.2 Principal Variation Search (PVS) and Aspiration Windows
- **Decision:** Employ PVS (full-window search on PV move, null-window `[alpha, alpha + 1]` on all subsequent moves) coupled with aspiration windows (`prevScore ± 25 cp`) on iterative deepening iterations beyond depth 1.
- **Why:** Most non-PV moves fail low. Null-window searches trigger early cutoffs without proving an exact upper bound, significantly reducing tree size.
- **Empirical Findings:** Aspiration windows reduce node counts by 18-28% on middle-game positions while fail-low/fail-high recovery guarantees score preservation.

### 3.3 Reverse Futility Pruning (RFP) and Razoring
- **Decision:** Implement shallow pruning guards: RFP at depth <= 2 when static evaluation minus depth margin exceeds beta, and Razoring at depth 1 when static evaluation plus 300 cp falls below alpha.
- **Why:** Avoids expanding doomed or overwhelmingly winning subtrees at horizons near leaf nodes.
- **Safety Guards:** Guarded against positions in check and endgame king-and-pawn positions (zugzwang risk).

### 3.4 Move Ordering Bands
- **Decision:** Implement strict disjoint move ordering bands:
  1. Transposition Table move (`order::kTTMove = 1 << 30`)
  2. Good Captures via MVV-LVA + Static Exchange Evaluation (`order::kGoodCapture = 1 << 26`)
  3. Promotions (`order::kPromotion = 1 << 25`)
  4. Killer moves (1st and 2nd killers, `order::kKillerFirst = 1 << 24`, `order::kKillerSecond = 1 << 23`)
  5. Quiet moves scored by History heuristic clamped below `(1 << 23) - 1`
  6. Losing Captures scored below quiet moves (`-(1 << 24)`)
- **Why:** Absolute disjoint bands prevent history bonuses from accidentally prioritizing quiet moves over winning captures or hash moves.

---

## 4. Transposition Table & Lifetime Management

### 4.1 Generational Aging vs Table Wiping
- **Decision:** Implement generational aging in `TranspositionTable` (`newSearch()`), incrementing the generation counter on each move search rather than zeroing out memory (`clear()`).
- **Why:** Clearing the TT between moves in a tournament game throws away valuable transpositions discovered during previous searches, drastically increasing the node count for transposing lines.
- **Replacement Policy:** An entry is replaced if:
  - Hash collision (different key).
  - Stale entry (different generation).
  - Deeper or equal depth search (`incoming.depth >= existing.depth`).
- **Verification:** Verified by `testTTPersistenceAcrossMoves` in `tests/search_test.cpp`, ensuring search node counts decrease on transpositions while scores remain invariant.

### 4.2 Full 64-Bit Zobrist Keys in TT Entries
- **Decision:** Store the complete 64-bit Zobrist key in each 16-byte `TTEntry` rather than relying on a 16-bit or 32-bit partial key.
- **Why:** Partial keys risk silent hash collisions where an evaluation from an unrelated board is accepted as valid, leading to catastrophic blunders. Full 64-bit verification prevents corrupt probes.

---

## 5. Software Architecture and Build Infrastructure

### 5.1 Standalone C++20 with Zero External Libraries
- **Decision:** The core engine, UCI protocol parser, and evaluation modules are written in standard C++20 using only STL headers and compiler intrinsics.
- **Why:** Eliminates runtime dependency issues across platforms, minimizes binary size, and ensures zero-overhead compilation with GCC / Clang on Windows and Linux.

### 5.2 Decoupled Python Match and Training Harness
- **Decision:** Keep the training harness (`nnue/`), dataset generation scripts, and match runner (`match/`) cleanly separated from the engine source (`engine/`).
- **Why:** The engine produces a clean, self-contained standalone binary (`pocketchess.exe`) for competition or hackathon evaluation, while tools interface strictly through the universal UCI standard.
