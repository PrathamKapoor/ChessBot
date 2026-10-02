# Research baseline

Snapshot: 2026-10-02. Every claim below was checked against a primary source
during this session. Where a widely-repeated claim turned out to be wrong, the
correction is recorded rather than quietly dropped.

## Phase 2: chess-core performance

Phase 2 optimised the verified Phase 1 core for nodes/second without changing
any chess semantics. Summary, then full experiment history.

### Baseline conditions

```text
Hardware:     AMD Ryzen 7 7435HS (Zen 3), 16 hardware threads, 24 GB RAM.
              No discrete GPU used by this phase.
ISA:          AVX2 + BMI2 (PEXT) confirmed by test; no AVX-512.
Compiler:     g++ 15.1.0 (MSYS2 UCRT64)
Build:        -std=c++20 -O3 -DNDEBUG -mavx2 -mbmi -mbmi2 -mpopcnt
              -funroll-loops   (build.ps1, identical before and after)
Workload:     six standard perft positions at depth 5 = 480,105,445 nodes.
Method:       build.ps1 (identical flags throughout)
              perft measured directly, best and median of 3 runs.
Phase 1 NPS:  36,811,838 median, 37,155,370 best  (13.04 s / 12.92 s)
```

### Phase 2 result

```text
Phase 1:  480,105,445 nodes / 13.042 s = 36,811,838 nps (median of 3)
Phase 2:  480,105,445 nodes /  5.085 s = 94,412,404 nps (median of 3)
          best of 3: 5.048 s = 95,101,200 nps
Gain:     +156.5% median, +155.9% best   (2.57x)
```

Per-workload, measured with `tools/ab.ps1` which interleaves the two binaries
and reports the best of 5:

| workload | base nps | phase 2 nps | delta |
|---|---|---|---|
| startpos d5 | 35,133,299 | 83,026,748 | +57.6% |
| kiwipete d4 | 36,863,761 | 90,797,809 | +59.7% |
| endgame-rook d6 | 29,343,147 | 55,671,315 | +47.3% |
| promo-castle d5 | 34,354,465 | 84,417,767 | +59.2% |
| promo-heavy d5 | 35,067,719 | 88,741,737 | +60.5% |
| positional d5 | 37,679,874 | 102,810,648 | +63.3% |
| **sum** | **208,442,265** | **505,466,024** | **+142.5%** |

### Where the time actually goes

The bottleneck moved, so the profile is recorded twice.

**Phase 1 profile** (`bench --reference`, all six cases, 223,331 positions,
34.09 legal moves per position):

```text
  component                            cyc/pos    share
  node own work (TOTAL)                10215.0   100.0%
  pseudo-legal generation                556.6     5.4%
  legality filter                      2718.5    26.6%
    legality: non-king/ep/castle       2517.5    24.6%
    legality: king moves                181.3     1.8%
    legality: castling                   17.1     0.2%
  do/unmake (incl. check detect)       ~6400     ~62%
  measurement overhead                 ~1500     ~15%
```

The Phase 1 assumption, that legality filtering would be cheap to fix once the
check-square machinery existed, was right, but not for the reason expected: the
cost was one `attackersTo()` call per pseudo-legal move, about 36 per position.

**Phase 2 profile** (`bench --profile`, same positions):

```text
  component                            cyc/pos    share
  node own work (TOTAL)                 6403.8   100.0%
  movegen (generation + legality)       1192.9    18.6%
  do/unmake (incl. check detect)       5062.1    79.0%
  measurement overhead                   148.9     2.3%
```

Movegen went from 3275 to 1193 cycles per position, a 2.7x improvement on that
component. do/unmake is now the bottleneck and, per the ablations below, it is
close to its floor: there is no single large win left in it.

### Experiment history

Every entry was validated with the six-position depth-5 perft suite plus the
differential divides against `python-chess` before being kept.

| ID | Hypothesis | Change | Correctness | NPS delta | Decision |
|---|---|---|---|---|---|
| P2-0 | per-move rdtsc attribution gives the bottleneck | built `tools/bench.cpp` | n/a | n/a | **rejected as a measurement method.** rdtsc costs 46 cycles here; two calls per `legal()` is ~3,300 cycles of overhead per node, swamping the signal. First profile attempt showed 85% "unattributed". |
| P2-1 | added-work ablation is trustworthy | `walkWork()` variants, identical traversal plus extra per-node work | n/a | n/a | **partly rejected.** Absolute cycles/node disagreed with the real benchmark by 6x, and the deltas overestimated `attackersTo` by ~7x. Added work in a tight loop cannot overlap with surrounding work; real work does. Kept as a rough cross-check only. |
| P2-A | `undoMove` recomputes a checkers value already known | store `checkers_` in `StateInfo`, restore the parent entry | perft OK, 480,105,445 | **+6.4% best / +3.1% median** (positional d5) | **accepted** |
| P2-B | legality can be decided per position, not per move | `Position::blockersForKing()`, pin test, check-evasion target masks, double-check reduces to king moves | perft OK, all six differential divides match | **+44.7% to +63.3%** across six workloads | **accepted** |
| P2-C | filtering through a second 1 KB buffer is waste | generate into the caller's buffer, compact in place | perft OK | **+12%** on top of P2-B | **accepted** |
| P2-D | check detection inside doMove is the remaining cost | ablation: replace `attackersTo` with `checkers_ = 0` | wrong by construction | **~0%** (1.798 s vs 1.783 s baseline) | **rejected.** Direct removal changes nothing measurable; added-work ablation had claimed ~24%. |
| P2-E | Zobrist updates are hot | ablation: `state_->key ^= 0` | wrong by construction | **+2.5%** | **rejected** as not worth the risk. |
| P2-F | castling-rights maintenance is hot | ablation: drop `disallowCastling` calls | wrong by construction | **+1.3%** | **rejected** as not worth the risk. |
| P2-G | fancy magic (8 PEXT to 2) would pay | not attempted | n/a | n/a | **not attempted.** P2-D showed `attackersTo` is nearly free in the real pipeline, so reducing its PEXT count has no headroom. Rejected on measurement, not on theory. |
| P2-H | fewer `StateInfo` fields would shrink the per-move copy | not attempted | n/a | n/a | **not attempted.** `pawnKey`, `nonPawnMaterial`, `pliesFromNull` and `move` are all consumed by later phases (NNUE halfKP, null move, repetition). P2-E/F suggest the copy is not the cost anyway. |

### Correctness bugs found while optimising

All of these passed perft in release builds and were caught by the gate.

1. **`checkers` restored from the wrong `StateInfo` entry.** P2-A initially read
   the child's entry instead of the parent's, giving 480,105,386 instead of
   480,105,445. Perft caught it on the first run.
2. **Castling tested after the generic king case.** The new `generateLegal()`
   checked `typeOf(pieceOn(from)) == kKing` before `isCastling`, so castling took
   the plain king path and skipped the path-empty requirement. startpos depth 1
   became 22 instead of 20.
3. **`betweenBB()` returned the whole rank minus the two endpoints** for a
   same-rank pair, including squares outside the span. In endgame-rook that made
   the one blocker between the b4 rook and the h4 king look like two, hiding the
   pin on f4. The first fix was worse: it ORed whole files for a same-rank pair,
   so `betweenBB(a5,h5)` returned 49 squares instead of 6. Now the span is walked
   explicitly and covered by `tests/geometry_test.cpp` over all 4096 pairs.
4. **`blockers |= s` where `s` is a `Square`.** Because `Square` is an
   enumeration with underlying type `int`, this selects the built-in integer
   `operator|=` and ORs in the square's *index* as a mask: `0x29` for b6 instead
   of `0x20000000000`. The pin mask came back as `a1, d1, f1, c5` and rejected
   every move off the first rank. Fixed to `|= squareBB(s)`.
5. **`tools/perft_diff.ps1` regex could not match 5-character UCI promotions**
   such as `d7c8q`, so promotions appeared as false positives in the diff.

### Guard added

Every operator mixing `Bitboard` with `Square` is now declared deleted, so
`bb |= sq` is a compile error rather than silently wrong. Note that removing the
mixing operators alone is *not* enough: the built-in integer operators are always
available and always win, so the deletion is what actually catches the mistake.
All existing call sites were made explicit with `squareBB()`.

The lesson generalises beyond chess: with `typedef uint64_t Bitboard` and an
`enum Square : int`, the two types overlap enough that ordinary arithmetic
operators accept each other. A scoped enum would remove the hazard entirely and
is worth considering in a later phase.

### Remaining bottlenecks

- **do/unmake, 79% of node cost.** Ablations show check detection, Zobrist and
  castling maintenance together account for under 4%, so the remainder is
  inherent: the `StateInfo` copy, bitboard updates and the `board_[64]` array
  writes. Further gains would need a different design (dropping `board_` for
  pure bitboards, or a slimmer undo representation), not local tuning.
- **movegen, 18.6%.** `blockersForKing()` runs once per position and the
  pseudo-legal loop runs `pext` per slider. Specialised in-check generation
  (target-mask pruning per piece type) would cut this but is a larger change
  with more chess-semantics risk, so it was left for a phase that can afford
  the validation cost.
- **Pseudo-legal generation still builds a `Move` array.** Staging moves or
  generating directly per piece type would reduce the 34-move array traffic but
  complicate the search integration that comes next.

## Standing rule

Nothing gets merged because an LLM, a developer, a benchmark, or a lower
validation loss says it is better. It gets merged because controlled chess
matches demonstrate it is better, under SPRT.

## Corrected baseline claims

The engines below were cited as prior art. Verified status:

| Claim | Status |
|---|---|
| **Tarnished** — elaborate NNUE, large training corpus | **Real.** `Bobingstern/Tarnished`, GPL-3.0, C++. Architecture `(768x16hm→1792)x2→(16→32→32)x8`, Bullet trainer, self-generated data, 16 king buckets, 8 output buckets, horizontal mirroring, Finny tables, lazy updates, fused refresh, dual activation, SPSA tuning, FRC support. #14 on SP-CC as of 2026-02-13; CCRL Blitz 3728 at v5.0. |
| **Sykora** — ~3240 CCRL, NNUE training/export pipeline | **Correct.** `sb2bg/sykora`, **MIT**, Zig, Bullet-trained embedded NNUE. 3240 CCRL / rank 163 at 0.2.2. |
| **Tcheran** — horizontally mirrored NNUE, self-play only | **Correct, and understated.** `(768x8hm→1024)x2→(16→32→1)x8`. v13.0 is **CCRL 40/15 3563 / Blitz 3684** — genuinely top-tier, not a 3000s engine. License **AGPL-3.0**. |
| **Hobbes** — "reports a top-20 rating around 3750", architecture `((768x16+60144)hm→768)x2→(16x2→32→1)x8` | **Rating is a forum estimate, not a measurement.** The only traceable source is an open-chess.org thread reading "Estimated: 3715 CCRL Blitz". No CCRL entry, and the architecture could not be verified at all. **Do not design against it.** |
| **casanchess** — "explicitly emphasizes incremental development and strict SPRT testing" | **Wrong.** Its README emphasizes *readability*; it is single-threaded with multithreading listed as a TODO, and it is a much weaker engine. MIT licence, but not a testing-methodology exemplar. The real exemplars are **OpenBench** and **Fishtest** — Tcheran literally forks OpenBench. |

Additional verified engines found during this session, useful as architecture
references: `Quinniboi10/Prelude` (`(768→1024)x2→1x8`, SCReLU, QA=255 QB=64),
`A1exL1ang/Superultra` (`(768x10→512)x2→1`, 10 mirrored king buckets, 8 output
buckets), `Quanticade/Quanticade` (C, self-play NNUE, CCRL Blitz 3731).

## Two technical corrections that change the design

**1. `768 → 512 → 1` is not a real NNUE architecture.** Actual topology is
`[features] → feature-transformer ×2 → narrow hidden layers → 1 × out-buckets`.
The feature transformer dominates both size and cost. For calibration,
Stockfish moved `256x2→32→32→1` → `1024x2→8→32→1`.

**2. The comparison table in the original brief sits below the current SOTA
baseline.** Stockfish 18 (SFNNv10) uses **HalfKAv2_hm + FullThreats with a
1024-d big FT and a 128-d small FT**, switching nets at runtime. Every
architecture in the proposed NNUE-v1..v4 ladder is smaller and shallower than
what Stockfish already ships. If the research question is "which architecture is
strongest", the honest starting point is "reproduce SFNNv10 exactly, then
attack it" — not `768→512→1`.

Also: king-bucketing multiplies the feature-transformer width (hence
`768x16hm→1792`); it does not turn `768` into `1024`.

## The strategic reframe

The original brief pushed toward "go bigger". On the actual hardware available —
16 hardware threads of a mobile Ryzen, 24 GB RAM, one RTX 4050 with 4 GB VRAM,
and no distributed testing capacity — **architecture capacity is not the
binding constraint. NPS and data volume are.**

A larger net costs depth *and* overfits because we cannot feed it. So the thesis
inverts:

> At fixed compute, what is the optimal (architecture, data-mix) trade-off?

This is laptop-appropriate, cheap to validate, and underexplored — because
everyone with Fishtest-scale compute never has to ask it. It is also the one
place a from-scratch engine on this hardware can plausibly *win* rather than
trail.

Concretely: the architecture ladder runs small-and-wide → narrow-and-deep, and
"bigger" is treated as a cost to be paid for, not a goal.

## Honest expectation

Beating Stockfish at equal resources is not attainable on this machine.
Fishtest runs on the order of 10^9 games per year on distributed hardware; we
cannot out-volume it. The realistic ceiling here is roughly 2800–3000 Elo, and
the achievable research contribution is *cheap-to-validate novel ideas*, not
brute-force tuning.

The literal goal "beat Magnus Carlsen and Bobby Fischer" cannot be validated:
Fischer is deceased, controlled matches against either are impossible, and
elite-human strength is not interchangeable with engine Elo. The defensible
target is **an independently measured superhuman engine rating, plus a
demonstrated pipeline for statistically validated improvement**.

## Gaps in the original plan worth closing

- **No Syzygy in the engine.** Fathom is MIT and roughly 1000 lines. Cheapest
  large Elo gain available.
- **No PGO / SPSA parameter tuning.** The single highest Elo-per-engineer-hour
  lever in the field; Tarnished ships `weather-factory` support for it.
- **No contempt / match-play design**, which is real Elo against specific
  opponents.
- **"Evolutionary population" is already OpenBench/Fishtest.** Rebuilding it
  locally is lower value than contributing patches back to those projects.
- **"Search-aware training" and "tactical curriculum" are research bets, likely
  negative.** Self-play naturally over-samples tactics, and hand-built curricula
  have a poor track record in game AI. They are legitimate to run, but must be
  SPRT-gated like everything else and must not be presented as a strength path.

## What this repository does instead

Phase 1 is complete: bitboards, position, movegen, and a perft gate that passes
all six standard positions (see the commit message for exact node counts).

Move generation is currently the slow, obviously-correct path — pseudo-legal
generation filtered through `Position::legal()`. That was deliberate: perft is
the correctness gate, and a pin/check-evasion filter is only worth writing once
perft is green. It measures ~33M nps, which is well below where this core should
end up.

## Debugging method that worked

`tools/perft.cpp` supports `--divides` and `--tree`, emitting machine-readable
CSV of per-move and full move-tree counts. `tools/reference_perft.py` and
`tools/reference_tree.py` enumerate identical trees with `python-chess`, so
divergences are **diffed rather than guessed at**.

This is what caught the `attackersTo` slider-type bug and the double-push
detection bug. Reasoning about "which move looks wrong" from the counts alone
wasted time repeatedly; diffing whole move trees against a reference found every
remaining bug immediately. Anyone continuing this work should reach for
`perft_diff.ps1` before reading engine source.
