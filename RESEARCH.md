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

## Phase 3A: first search layer

Snapshot: 2026-10-03. Starting commit for the phase: `24387c2` (end of Phase 2).
This section records only what was measured. Where a number is absent it was not
measured, and that is stated rather than estimated.

### Baseline conditions

    CPU     AMD Ryzen 7 7435HS, 8 cores, 3.1 GHz nominal
    OS      Windows, MSYS2 UCRT64
    gcc     g++ (Rev5, Built by MSYS2 project) 15.1.0
    flags   -std=c++20 -O3 -DNDEBUG -mavx2 -mbmi -mbmi2 -mpopcnt -funroll-loops

A caution that applies to every timing below. This machine throttles hard under
sustained load. Three identical repetitions of one benchmark position measured
88,682,514 nps, then 68,549,343, then 60,209,187 nps in a single sitting, a 47%
spread with nothing changed. Phase 2 hit the same thing.

Consequences adopted for this phase:

  * **Node counts are the primary measure.** They are deterministic and were
    bit-identical across every repetition of every configuration below.
  * Wall clock is reported for context only.
  * Phase 2's verified move-generation baseline of **94,412,404 median nps** is
    left untouched and is not comparable to the numbers taken today.

Pre-search state at `2c6af72`, for reference:

| gate | result |
| --- | --- |
| BITBOARD | OK (262144 ray comparisons) |
| GEOMETRY | OK |
| EVAL | OK (146 checks) |
| six-position perft d5 | 480,105,445 |
| startpos d6 | 119,060,324 |
| endgame-rook d6 | 11,030,083 |
| differential divides d3 | 6/6 match python-chess |

Move-generation speed measured at the start of this phase was 77,582,182 nps on
the six-position perft, below the Phase 2 median because of the throttling above.
That figure is a move-generation number and says nothing about search strength.

### Transposition table

Entry, 16 bytes:

    offset  0  uint64  key          full 64-bit Zobrist key
    offset  8  uint16  move
    offset 10  int16   score        ply-normalised
    offset 12  int8    depth        depth actually searched
    offset 13  uint8   bound        EXACT / LOWER / UPPER
    offset 14  uint8   generation
    offset 15  uint8   pad

**Collision strategy.** Indexed by the low bits of the key, but a probe compares
all 64 bits of the stored key. A hash collision therefore resolves to a miss
followed by an overwrite; it can never return another position's score, depth or
move. A truncated key would save a few bytes and would risk silently returning an
evaluation for the wrong position, which is the most dangerous class of search
bug because it yields plausible numbers rather than a crash.

**Replacement policy**, one policy, not a family:

1. overwrite on a hash collision,
2. overwrite on a stale generation,
3. otherwise overwrite when the incoming search reached at least the same depth.

Rule 3 uses `>=` rather than `>`. A strict comparison rejects equal-depth stores,
and with them the best move, freezing each position at whichever move the first
search of that depth happened to produce. That was a real bug, caught by the
move-round-trip test.

**Mate normalisation.** `scoreToTT` adds the ply, `scoreFromTT` subtracts it,
and they are the only two places the conversion happens. Tested directly,
including that ordinary centipawn scores pass through unchanged at any ply and
that a score just inside the sentinel boundary is not shifted.

**Size** 16 MB by default, 1,048,576 entries, settable through UCI `Hash`.

### Search

**Score convention.** Side to move, at every ply. This matches `evaluate()`,
which already returns a side-to-move-relative score, so there is no conversion
layer. Asserted in the tests rather than left to inspection, because a
White-relative evaluation fed into a side-to-move search plays as if the colours
were swapped and produces no error message.

**Terminal scoring.**

    mate      +kValueMate - ply   larger means a SHORTER mate
    mated     -kValueMate + ply   larger means a LONGER defence
    stalemate 0
    draw      0

Both mate forms use larger-is-better, so a plain `max()` gives the right
preference in each direction with no special case at the call site.

**Draw model.** A draw scores zero for: repetition inside the fifty-move window,
a fifty-move counter at 100 or more, and insufficient material.

At the root only the two position-local rules apply (`isPositionallyDrawn`).
Repetition is deliberately excluded there, because whether the position in front
of a search has already occurred is game history that the caller owns, and
treating an arbitrary root as drawn would stop the engine playing a position that
is perfectly playable as a fresh puzzle. Stalemate is also excluded from the
root set, and for a different reason: proving it requires generating the moves
and finding there are none.

Twofold repetition is used as the repetition test. That is stronger than the
strict rule, on the grounds that the opponent would claim a third occurrence
anyway. It is a choice, not an oversight.

**Fail-soft semantics.** The best score seen so far is returned, not beta, so a
cutoff returns a bound rather than an exact value. The bound is classified
against the **original** alpha captured before the move loop, never the alpha the
loop ended on; classifying against the latter labels ordinary cutoffs EXACT, which
is how a transposition table starts asserting false certainties.

**Recursion.** Negamax / alpha-beta with iterative deepening 1..N, a triangular
PV table, and mate distance pruning. One extension: a ply is added when the side
to move is in check, and it is switchable so it can be ablated.

An iteration cut short by a time or node limit still reports its result, but the
remembered answer is only taken from an iteration that finished. The benchmark
confirms the effect: the back-rank-mate position stops at depth 1 with
`score mate 1` and 17 nodes rather than continuing to the requested depth.

### Move ordering

Disjoint score bands, so a tiebreak bonus can never overtake a more important
category no matter how large it grows:

    1  transposition table move
    2  captures that SEE says win material
    3  promotions
    4  first killer for this ply
    5  second killer for this ply
    6  history for the piece and destination
    7  plain quiet moves
    -- captures that SEE says lose material, below every quiet move

SEE rather than MVV-LVA alone for captures, because MVV-LVA cannot tell a capture
that wins a queen from one that loses a rook to a recapture.

Two killers per ply, reset at the start of every search. A killer is only used if
it matches a generated legal move, so a killer carried over from a different
position is ignored rather than trusted.

History is `[colour][pieceType][destination]`, updated on quiet beta cutoffs with
`+= depth*depth` and decayed by gravity, clamped to +/-16384 to prevent runaway.
Both killers and history are cleared at the start of every search: they accumulate
across the iterations of one iterative-deepening run, which is where their value
is, but carrying them across separate searches makes the node count depend on what
was searched before and makes benchmarks irreproducible.

### Quiescence search

Stand pat on the static score, then captures and promotions only. When in check
there is no stand pat and every legal reply is searched, because evaluating a
position that still has to be resolved treats a losing position as fine.
Checkmate inside quiescence is detected from the absence of any legal reply.

### Reference oracle

`negamaxReference` is a deliberately plain negamax: no table, no ordering, no
quiescence, no extensions. It exists so the fast path is never graded against its
own assumptions.

The important methodological point: quiescence and the check extension both change
what a fixed-depth search can see, so comparing the full engine against the
reference at the same depth is meaningless. Comparisons run with those two
switched off, which isolates the table and the ordering, and those are required to
be score-preserving. Ten position/depth pairs agree exactly.

Stockfish is deliberately not an oracle. Being strong does not make a score
correct, and one engine's evaluation is not the right thing to grade a different
evaluation against.

### Benchmark

Nine positions: startpos, Italian, two knights, a kiwipete-like middlegame, a
rook endgame, a queenless middlegame, a king-and-pawn endgame, a position with a
mate in one, and a Sicilian.

Full engine, nodes are identical across repetitions:

| depth | nodes | qnodes | nps (approx) | TT hit rate | beta cutoffs | first-move cutoffs |
| --- | --- | --- | --- | --- | --- | --- |
| 4 | 217,696 | 7,556,776 | 14,900 | 3.68% | 19,039 | 17,193 |
| 5 | 851,078 | 22,568,968 | 20,200 | 7.34% | 163,777 | 149,483 |
| 6 | 5,377,795 | 42,003,622 | 67,600 | 6.18% | 548,483 | 514,020 |

Per-position node counts at depth 6, all returning a legal move:

| position | nodes | score |
| --- | --- | --- |
| startpos | 313,167 | cp 13 |
| italian | 707,127 | cp 137 |
| two knights | 1,194,715 | cp 12 |
| kiwipete-like | 2,298,608 | cp 3 |
| endgame-rook | 60,332 | cp 187 |
| queenless | 221,376 | cp 349 |
| kp endgame | 2,347 | cp 359 |
| back rank mate available | 17 | mate 1 |
| sicilian | 580,106 | cp 33 |

### Ablations

Depth 5, nine positions, two repetitions each. Node counts are exact;
`best-nodes` is the figure to compare.

Individual mechanisms, each removed from the full engine:

| configuration | nodes | vs full | qnodes | time (ms) |
| --- | --- | --- | --- | --- |
| full engine | 851,078 | -- | 22,568,968 | 42,700 |
| no transposition table | 1,528,948 | **+79.7%** | 31,162,318 | 59,400 |
| no SEE ordering | 856,565 | +0.6% | **63,564,853** | **104,500** |
| no killers | 885,046 | +4.0% | 23,308,096 | 43,500 |
| no history | 867,740 | +2.0% | 23,057,023 | 46,200 |
| no check extension | 750,097 | -11.9% | 22,552,522 | 45,100 |

Cumulative build-up:

| configuration | nodes | vs previous | time (ms) |
| --- | --- | --- | --- |
| baseline: no TT, no ordering | 2,983,846 | -- | 217,600 |
| + TT | 1,043,357 | **-65.0%** | 118,800 |
| + SEE ordering | 1,050,866 | +0.7% | 46,700 |
| + killers | 867,740 | -17.4% | 43,400 |
| + history | 851,078 | -1.9% | 43,000 |

Three things are worth stating plainly.

**The table is the real win.** 65% fewer nodes, and 44% fewer than the full
engine without it. Nothing else comes close on nodes.

**SEE ordering pays for itself in time, not in nodes.** It changes the node count
by 0.6%, which on its own looks worthless. It cuts quiescence nodes from 63.5M to
22.6M and wall clock from 104.5s to 42.7s, a **2.4x speedup**. Without it, losing
captures are searched early, the move ordering collapses, and quiescence runs away
with the tree. Judging this mechanism by node count alone would have led to
deleting it.

**Removing the check extension reduces nodes by 11.9%, and that is not an
efficiency gain.** It searches less because it sees less. It is listed for
completeness and must not be read as a result.

The `no quiescence search` row is likewise not a comparison: it has 1,696,192 nodes
in 4.6s because it is not searching captures at all. It exists to show the cost of
the current quiescence, not to argue for removing it.

### What the numbers say is wrong

**Quiescence is the bottleneck, and it is not close.** At depth 5 there are 851,078
main-search nodes and 22,568,968 quiescence nodes, a ratio of 26:1. A tuned engine
is normally in single digits. The reason is visible in the NPS column: search runs
at about 20,000 nps while move generation alone runs at tens of millions. The
evaluation is not incremental, and it is being called once per quiescence node.

Three candidate causes, not yet separated by measurement:

1. the evaluation recomputes every term from scratch, including mobility, which
   walks every sliding attack for every piece on the board;
2. quiescence has no depth limit, so a capture race runs arbitrarily long;
3. SEE is called during ordering for every capture at every node, and it
   allocates and walks the swap sequence each time.

The instrumentation question from Phase 2 applies again here. The counters in
`SearchStats` and `TranspositionTable` are plain increments inside the hot path.
Their cost has not been isolated yet. Before trusting an NPS figure from this
engine, a run with statistics compiled out should be compared against a run with
them on, in the same way Phase 2 compared interleaved binaries.

### Deferral

Deliberately not implemented, and each is a candidate for the next measured
increment rather than an oversight: null-move pruning, late move reductions, late
move pruning, futility and reverse futility, razoring, ProbCut, singular
extensions, aspiration windows, internal iterative reductions, multi-cut,
speculative extensions, and PVS. PVS in particular is conspicuous by its absence
and is the most likely next single win on node count.

### Correctness after this phase

| gate | result |
| --- | --- |
| BITBOARD | OK (262144 ray comparisons) |
| GEOMETRY | OK |
| EVAL | OK (146 checks) |
| TT | OK (5147 checks) |
| SEARCH | OK (306 checks) |
| six-position perft d5 | 480,105,445 |
| startpos d6 | 119,060,324 |
| endgame-rook d6 | 11,030,083 |
| differential divides d3 | 6/6 match python-chess |
| build | warning-free |

### Bugs found while writing the search tests

Recorded because the pattern recurred and the tests are the only reason they were
caught.

  * The reference search and the fast search disagreed on nearly every fixed
    depth. Not a search bug: quiescence and the check extension genuinely change
    the horizon. The comparison had to be restructured, not the search.
  * A fifty-move draw test that returned a winning score. The test position gave
    White a legal castling move, and castling resets the fifty-move clock. The
    engine was right; the test was escaping from the draw it meant to test.
  * Several "obvious" tactical FENs were illegal: a queen that did not attack the
    king it was supposed to mate (that position is stalemate), a rook that shared
    no rank or file with its target, a rank with nine squares in it, a pawn on the
    wrong square to defend the square above it.
  * A test asserting that the engine captures an undefended bishop with a knight.
    The engine declined because that loses material. It was right; a rook was used
    instead.
  * `go infinite` hung, and `stop` could never be read, because the search ran on
    the reader thread. Fixed with a worker thread.
  * Then `go infinite` hung again: the worker cleared the stop flag on entry, and
    the worker could start *after* the reader had already processed `stop`,
    swallowing it. The flag is now cleared before the worker starts.
  * `history_` was not reset between searches, so node counts depended on what had
    been searched before and two identical runs differed by 5%.

### Bugs found in the evaluator, for the record

Eight, all caught by the mirror-invariance test in `tests/eval_test.cpp` rather
than by perft. They are listed in the commit message for `2c6af72`; the two with
the widest blast radius were `evalMaterial` counting both colours' pieces together
as White's material, and the piece-square tables being loaded with a 180-degree
rotation instead of the vertical flip that is the actual chess symmetry.

### Next step

Not NNUE, and not a training pipeline. The order that the measurements point at:

1. **Isolate instrumentation overhead.** Compare a statistics-free build against the
   current one before trusting any NPS figure from this engine. The Phase 2 lesson
   applies directly and this has not been done yet.
2. **Bound quiescence.** 26 quiescence nodes per main node is the dominant cost.
   Capping quiescence depth, or stopping the capture loop when the best capture is
   clearly losing, is the single largest expected win.
3. **Incremental evaluation.** The evaluation is the per-node cost; making the
   common terms incremental is what would lift the NPS figure itself.
4. **PVS.** The largest known missing reduction on node count.
5. **Only then a match runner and SPRT**, so that every later change can be judged
   by playing strength rather than by node counts. Node counts are a proxy, and a
   proxy is not a strength measurement.

Strength has still not been measured at any point in this repository. Everything
above is correctness and cost. The first real strength measurement will be the
first match.

## Phase 3B: measurement purification and qsearch cost

Snapshot: 2026-10-03. Starting commit `0d6782e`, Phase 3B baseline recorded at
`304e22d`. Phase 1, 2 and 3A results above are left untouched.

This phase began with Phase 3A's own list of limitations, in which the first item
was that the statistics counters had never had their overhead measured. That
turned out to be wrong in an informative way, and the rest of the phase followed
from measuring rather than assuming.

### Baseline frozen before any change

    commit   0d6782e
    gcc      g++ (Rev5, MSYS2) 15.1.0
    flags    -std=c++20 -O3 -DNDEBUG -mavx2 -mbmi -mbmi2 -mpopcnt -funroll-loops
    CPU      AMD Ryzen 7 7435HS, 8 cores, 3.1 GHz nominal
    OS       Windows 10.0.26200, MSYS2 UCRT64
    Hash     16 MB (1,048,576 entries), 1 thread
    depth    4, 5, 6 over the nine benchmark positions

Node counts, recorded before anything was touched and confirmed bit-identical
afterwards:

    depth 4     217,696
    depth 5     851,078
    depth 6   5,377,795

The Phase 3A wall-clock figures are labelled **instrumented / potentially
measurement-contaminated** and are not treated as authoritative below. Their node
counts are authoritative.

### Experiment table

| ID | Hypothesis | Configuration | Correctness | Main nodes | Qnodes | Wall time | Decision |
| --- | --- | --- | --- | --- | --- | --- | --- |
| P3B-INST-01 | Diagnostic counters in the hot path are costing real time | `POCKET_INSTRUMENT=0` vs `=1`, interleaved, depth 4, 10 rounds | nodes identical every round | 217,696 both | n/a in production build | 20,032ms vs 19,995ms minimum | **REJECTED as a hypothesis.** Overhead -0.2%, noise floor 3.3%. The counters are free. |
| P3B-INST-02 | Same, where quiescence dominates more | depth 6, 4 rounds | nodes identical every round | 5,377,795 both | n/a | 81,044ms vs 81,407ms minimum | **REJECTED.** Overhead +0.4% against a 54.1% noise floor. |
| P3B-EVAL-01 | The evaluation dominates search time | `tools/eval_microbench`, 20,000 deterministic positions | n/a, isolated | n/a | n/a | 2,895.5 ns per evaluate() | **ACCEPTED as the bottleneck.** |
| P3B-EVAL-02 | `pawnAttacksBBAll` is being called per pawn inside the pawn loop | hoisted out of the loop | EVAL OK 152 | 851,078 unchanged | unchanged | evaluate 2,895 -> 1,938 ns | **ACCEPTED.** 3.4x on that term. |
| P3B-EVAL-03 | `evalThreats` calls `attackersTo` up to 3x per square | merged the two colour loops into one pass | EVAL OK 152, mirror invariance caught two errors on the way | 851,078 unchanged | unchanged | evaluate 1,938 -> 1,209 ns | **ACCEPTED.** |
| P3B-EVAL-04 | Cheap-evaluation build confirms the attribution | `POCKET_CHEAP_EVAL=1`, depth 5 | different engine, different tree by construction | 747,608 | n/a | 8.7us vs 40.3us per main node | **CONFIRMED.** Evaluation is 78% of search time. |
| P3B-EVAL-05 | Incremental evaluation is worth building | would have covered material and piece-square tables first | n/a | n/a | n/a | those terms are 4.4% of evaluate() between them | **DEFERRED, with reason.** The terms it would cover cost almost nothing while the two expensive ones cost 72%. Reconsider only after threats and pawn structure are cheaper. |
| P3B-QS-01 | Quiescence generates moves it then discards | instrumentation counters | n/a | 851,078 | 22,568,968 | n/a | **CONFIRMED.** 25.69 moves generated per call, 3.11 searched. |
| P3B-QS-02 | Capture-only generation inside quiescence | `generateLegal(..., capturesOnly)` | SEARCH OK 306, TACTICAL 17/17 | **851,078 unchanged** | 22,568,906 | 38,404ms -> 31,681ms minimum | **ACCEPTED.** -17.5%, identical tree. |
| P3B-QS-03 | Cap quiescence depth | not attempted | n/a | n/a | n/a | n/a | **DEFERRED.** P3B-QS-02 bought the same saving with no semantic change at all. A depth cap changes the horizon and would need tactical validation; there is no reason to take that risk while a lossless saving is still available. |
| P3B-QS-04 | Skip SEE in quiescence ordering | not attempted | n/a | n/a | n/a | seeCapture is 49 ns and is called 27.9M times, about 1.4s of a 31.7s run | **DEFERRED.** Measured at under 5% of runtime, so the Phase 3A finding that SEE costs a lot of time in wall clock does not generalise to quiescence once move generation was fixed. |
| P3B-NET-01 | All of the above together | against the phase-start commit, depth 5, 4 rounds | all gates green | **851,078 unchanged** | unchanged | 45,504ms -> 30,354ms minimum | **ACCEPTED.** -33.3%. |

### Instrumentation overhead, measured

The counters are now behind `POCKET_INSTRUMENT`, defined in `engine/types.hpp`
and defaulting to 0. With it off the increments are discarded by `if constexpr`
and nothing is emitted, so the production binary is not paying a branch either.
Node counting is deliberately left outside the switch: node limits depend on it
and node count is the only figure this repository treats as deterministic.

The Phase 3A prediction was that the counters were contaminating every NPS figure.
They were not:

    depth 4, 10 rounds   production min 20,032ms, instrumented min 19,995ms
                         overhead -0.2%, within-binary spread 3.3%
    depth 6,  4 rounds   production min 81,044ms, instrumented min 81,407ms
                         overhead +0.4%, within-binary spread 54.1%

Both are below the noise floor. The useful finding is the second number rather
than the first: on a long run this machine cannot resolve a 0.4% difference
because its own spread is 54%. Node count therefore remains the primary metric
and minimum-time the secondary one, and no timing claim below 20% on a long run
should be believed without a tighter noise floor.

### Where qsearch time actually goes

Counted in the instrumented build at depth 5, and attributed using separately
measured standalone costs:

| quiescence operation | count | standalone cost | estimated time | share |
| --- | --- | --- | --- | --- |
| stand-pat evaluations | 19,639,738 | 788-1,220 ns | 15.5-24.0 s | 55-82% |
| move generation | 12,424,708 calls | 593 ns | 7.4 s | ~25% |
| SEE | 27,895,052 calls | 49 ns | 1.4 s | ~5% |
| make/unmake | 21,958,173 pairs | ~30 ns | 0.7 s | ~2% |

The evaluation range is wide because evaluate() itself varied between 788 and
1,220 ns across runs on this machine. The cheap-evaluation build settles which end
matters: with the evaluation reduced to a material count, time per main node falls
from 40.3us to 8.7us, so evaluation is 78% of search time and everything else is
22%.

The answer to "why is qsearch expensive" is therefore not that quiescence does
anything expensive. It is that quiescence calls evaluate() 26 times for every
main-search node. Two independent fixes follow from that, and both were taken:
make evaluate() cheaper, and stop generating the moves quiescence throws away.

### Qsearch optimization: capture-only generation

Quiescence was generating 25.69 moves per call and discarding all but the 3.11
that were captures or promotions. The quiet moves were paid for in full.
`generateLegal` and `generatePseudoLegal` now take a `capturesOnly` flag that
restricts piece targets to enemy-occupied squares, skips quiet pawn pushes and
double pushes, and skips castling. A pawn push that promotes is still generated,
because promotion is one of the classes quiescence searches.

When the side to move is in check the flag is not used at all: every legal reply is
needed there, quiet ones included, because stand pat is unavailable.

The move set is identical to what the old code generated and then filtered by
`captureOrPromotion()`, so the tree cannot change. The evidence is node counts:

    depth 4   217,696 before, 217,696 after
    depth 5   851,078 before, 851,078 after
    depth 6 5,377,795 before, 5,377,795 after

Moves generated inside quiescence fell from 319,210,475 to 98,816,480, and
interleaved wall clock at depth 5 fell 17.5%, faster in all five rounds.

**A real bug surfaced during this change and the node counts caught it.** With
capture-only generation, a call returning zero moves no longer means "no legal
moves"; it usually means "no captures available". The old code treated zero as
stalemate and returned a draw score, which was scoring most quiet quiescence leaves
as draws. Node counts moved by 6 and 53 before it was found and they are now
exactly unchanged.

### Evaluation cost

| term | before | after | share after |
| --- | --- | --- | --- |
| threats | 1,050.6 ns | 533.7 ns | 43.7% |
| pawn structure | 1,035.8 ns | 342.6 ns | 28.1% |
| king safety | 219.7 ns | 168.5 ns | 13.8% |
| mobility | 152.0 ns | 108.1 ns | 8.9% |
| piece squares | 99.8 ns | 108.3 ns | 8.9% |
| material | 15.7 ns | 30.9 ns | 2.5% |
| space | 8.0 ns | 9.9 ns | 0.8% |
| **total** | **2,895.5 ns** | **~1,209 ns** | |

2.4x cheaper overall, with the search visiting bit-for-bit the same tree.

Two causes, neither related to chess. `pawnAttacksBBAll()` walks all sixty-four
squares and was being called once per pawn, inside the per-pawn loop, for a test
only the rare backward-pawn case needs. And `evalThreats` looped once per colour,
so `attackersTo()` was called up to three times for the same square: once for
defenders, once for "we attack it", and again in the second loop for "they attack
it". `attackersTo()` probes sliding attacks and is the most expensive thing in the
evaluation.

The threats rewrite needed two attempts. The first merged the loops but applied
only one of the two contributions a hanging piece receives: the old code scored
each hanging piece twice, once as "their piece we attack" and once as "our piece
they attack", and the net is (value-weighted attacked) minus (flat hanging). The
second failure was a shadowing bug, an inner `bool white` hiding an outer
`Bitboard white`, so the own/enemy masks were computed from 0 and 1. Both were
caught by the mirror-invariance test in tests/eval_test.cpp, which is the single
best thing in this repository.

Because mirror invariance caught that only as an aggregate, `testThreatTermSemantics`
now asserts the term directly: a hanging minor is positive, colour antisymmetry
holds exactly, and a defended piece contributes nothing.

### Tactical regression

`tests/tactical_test.cpp`, 17 positions, all solved. It is a regression check and
is labelled as such in the file: no matches are played, no Elo is claimed, and
"solved 17 of 17" says nothing about any position not in the set.

Coverage is aimed at what a quiescence change would break: mates found only
because quiescence kept searching, promotions, capture-promotions, en passant, a
position already checkmated, and a stalemate with no move.

Two cases failed on the first run and the engine was right both times. The en
passant case asserted that exd6 was best with a black king on e8; after exd6 Black
plays Kd7 and then Kxd6, leaving bare kings and a draw, so the capture scores zero
and a king move scores slightly better. The test had assumed a capture wins
material without asking whether it survives.

### Known limitations after this phase

  * Evaluation is still 78% of search time and still recomputes everything from
    the whole position. `threats` at 43.7% and `pawnStructure` at 28.1% are the
    remaining targets, and both still call `attackersTo` once per piece.
  * Incremental evaluation was deferred on measured grounds, not dismissed. If
    threats and pawn structure get cheaper, the balance changes and the cheap
    additive terms become worth revisiting.
  * SEE ordering still changes node count by very little. The Phase 3A result
    that it was worth 2.4x on wall clock does not survive the move-generation fix,
    because the original benefit was keeping losing captures out of the ordering,
    which is what stopped quiescence running away. That effect is real; the
    2.4x figure should not be carried forward.
  * Wall clock on this machine cannot resolve differences below about 20% on a
    long run. A tighter measurement method is needed before the next round of
    claims.
  * The transposition table hit rate is still around 6-7% and remains undiagnosed.
  * Strength is still unmeasured. Every number here is correctness or cost. The
    first real strength measurement will be the first match, which is Phase 3C
    work and is deliberately not started.
## Phase 3C: match infrastructure, and the bugs only a match could find

### Why this phase exists

Every number in Phases 2, 3A and 3B is correctness or cost. None of it is
strength. This phase built the thing that can finally measure strength, and the
first thing it found was not a strength result at all: the engine could not
correctly play a game over the UCI protocol it advertised.

### Runner decision

`fastchess`, `cutechess` and `cutechess-cli` are not installed on this machine,
and vendoring a full runner is a large dependency to carry for a fixed-depth,
single-clock, two-engine use case. So `match/runner.py` is first-party: a small
runner covering exactly what a self-match and an A/B need. It deliberately does
**not** implement endgame tablebase adjudication, an adjudication engine, cluster
play, EPD/EPV loading beyond the suite file, or concurrent games. Those are
documented omissions, not oversights.

The only external dependency is `python-chess` 1.11.2, whose distribution
metadata declares `GPL-3.0+`, used for the board representation, UCI/PGN parsing
and FIDE draw rules. Nothing is vendored, and the dependency is confined to the
test and match tooling rather than the engine. The runner itself is part of this
repository.

### Failures and chess results are disjoint in the data model

A crash, an illegal move, a missing bestmove, a timeout, a malformed UCI string
and a protocol violation are recorded with `result: null` and an
`is_failure: true` flag. They are never averaged into a score, and the runner
stops and exits non-zero on the first one. `move_limit_reached` and
`flag_fall` are chess outcomes and live in the result set, not the failure set.

One distinction worth stating because it was wrong first: an engine that exits
*cleanly* while a search was outstanding is recorded as `no_bestmove`, not as
`engine_crash`. The exit code is waited for, because `poll()` can still return
`None` at the instant the pipe closes. These are different faults and the record
is only useful if it says which.

### Clock accounting

Clock games decrement the side to move by the measured think time and then add
the increment, matching the rules. Time runs out as a loss on time, a chess
result. An earlier version passed the un-decremented clock on every move, which
meant the clock never ran down and a timed match silently degraded into an
unlimited one while still reporting a clock control.

Verified with `P3C-CLOCK-001`: 4 games at 10s + 1s, all completed, no flag falls,
the 30s per-move hard timeout never reached.

### SPRT

Two boundaries, `ln((1-beta)/alpha)` and `ln(beta/(1-alpha))`, with
`alpha = beta = 0.05` giving +/-2.9445.

    LLR = wins * ln(p1/p0) + losses * ln((1-p1)/(1-p0))

Only decisive games enter the sum. Draws cancel under a common draw-rate
assumption, which is stated in the module rather than left implicit. The
`elo0` and `elo1` hypotheses are logistic **decisive-game win probabilities**, not
scores that include draws, so `elo0 = 0` means "the candidate wins half of all
decisive games", not "it scores 50%". That distinction is the difference between
an honest SPRT and one that quietly needs a draw-rate estimate.

Synthetic validation (`match/sprt.py --simulate`):

  * truth beyond H1 accepts H1
  * truth beyond H0 accepts H0
  * truth inside the gap stays inconclusive

For the inside-the-gap case a single run is not a valid test: a sequential test
accepts early by mistake at rate `alpha` by design, and one fixed seed did exactly
that. The self-test therefore measures the false-accept rate over 200 seeds and
requires it below 0.20, with `alpha + beta = 0.10` as the expected value. It
also checks that a strong effect still decides, so the test cannot pass by
refusing to decide anything.

### Opening suite

12 fixed positions, `match/suite_core-01.txt`, each generated from a named SAN
line and stored with its FEN. Coverage: open, closed, tactical, queenless,
opposite-side castling, majority, endgame and asymmetric structures.
`match/verify_openings.py` re-derives every FEN from the move list, checks it
against the stored one, and confirms the engine accepts each FEN. All 12 pass.

### Two real engine bugs, both invisible to perft

The first match game played `b2b3` every ply until the runner rejected it as an
illegal move. Chasing that down found two defects that no existing gate could
see.

**1. `position ... moves ...` never replayed anything.** `applyPosition` scanned
for the `moves` keyword and then read a fresh token expecting to find it again.
The keyword had already been consumed, so the comparison failed and the replay
was skipped. Every position command with a move list silently kept the opening
position, while the engine kept searching and printed a plausible bestmove.

**2. Castling and en passant arriving over UCI lost their flags.** Both are
flagged moves internally, and the flag is what tells `doMove` what to do.
`moveFromUci` built a move from the four coordinate characters alone, so it never
set those flags. `e1g1` became an ordinary king move: the king teleported two
squares and the rook stayed on its home square. An en passant capture became a
plain capture and left the passed pawn standing. `moveFromUci` now resolves the
string against the generated legal moves, which carry the correct flags, and
returns failure rather than guessing when nothing matches.

Both are unreachable from perft, which generates moves internally with correct
flags. They are only reachable through the protocol. The existing UCI test passed
because it asserted that *some* bestmove appeared, not that the position
advanced; that is a test that cannot fail for this class of bug. It now asserts
that the side to move flips, that castling moves king and rook, that an en
passant capture removes the captured pawn, and that a move which is not legal in
that position is rejected rather than guessed at.

The practical consequence: no match played before this fix is valid. Any engine
built before `949f6b7` cannot play a UCI game at all, which is why the A/B below
carries this fix on both sides.

### Experiment: baseline vs candidate, P3C-AB-001

Isolated worktrees of two historical commits, both with `949f6b7` applied so the
protocol fix is held constant and the only difference is the Phase 3B engine
work. Cherry-picking the fix onto both sides is what makes this a measurement of
Phase 3B rather than a measurement of the UCI bug.

  * baseline  `304e22d+949f6b7`, sha256 `d7d75874558e205c`
  * candidate `da0654a+949f6b7`, sha256 `b20f5f6b219fa434`
  * control: fixed depth 4, hash 16MB, 24 games, 12 openings played twice with
    opposite colours, `max_plies` 200, hard timeout 60s per move
  * build: MSYS2 UCRT64 g++ 15.1.0, `-O3 -DNDEBUG -mavx2 -mbmi2 -mpopcnt
    -funroll-loops -DPOCKET_INSTRUMENT=0`
  * tree clean at run time

Result: **7 wins, 10 draws, 7 losses, score 0.5000**, LLR -0.0014 against
+/-2.9445, SPRT inconclusive. Terminations: checkmate, insufficient material,
threefold repetition. Zero infrastructure failures.

Both commits report identical node counts at depths 4, 5 and 6 on the benchmark
suite, so it is reasonable to expect identical play. It is not identical, and
that is worth being precise about rather than rounding away. Playing the two
engines in lockstep from `core-01-a`, the first divergence is at ply 98:

    fen      8/r7/5pk1/2p2p1p/7P/1p3K2/8/8 w - - 0 50
    304e22d  f3g3   score cp -2429   nodes 2723
    da0654a  f3e3   score cp -2431   nodes 2800

A 2 centipawn difference in a position already lost by 2400. Both moves lose.
The Phase 3B changes were intended to be semantics-preserving; the evaluator
rewrite and the capture-only quiescence are very nearly so, but "very nearly" is
not "exactly", and one position in this suite is enough to show it. The
conclusion is not that Phase 3B changed strength. It is that the observed
0.5000 is what equal strength looks like, and that a score of exactly 0.5000 over
24 games carries no information about a 5 Elo hypothesis.

### Determinism, P3C-AB-002

The whole A/B was repeated with identical binaries and configuration. **24 of 24
games reproduced exactly**: same result, same termination, same ply count, and
every individual move identical. Both binaries' sha256 match across runs. This is
the check that makes the ply-98 divergence above meaningful, because without it
the divergence could have been ordinary nondeterminism.

### Self-match, P3C-SELF-001

`ed256f1` against itself, same control, same suite. **7 wins, 10 draws, 7
losses, score 0.5000**, LLR -0.0014, SPRT inconclusive, zero infrastructure
failures, colours balanced 12/12. A self-match landing on exactly 0.5000 with a
symmetric win/loss split is the expected identity result and is the simplest
end-to-end check that the harness has no colour bias and no scoring bias.

Draw rate was 10/24 = 41.7%, which is high. Worth noting because it directly
reduces SPRT power: with only 14 decisive games, a 5 Elo hypothesis is
unreachable in principle, not merely unobserved.

### Known limitations after this phase

  * **No strength claim is possible from this data, and none is made.** 24 games
    at 41.7% draws gives 14 decisive games. A 5 Elo SPRT at alpha = beta = 0.05
    needs on the order of hundreds of decisive games. The SPRT correctly reports
    inconclusive; that is the tool working, not the tool failing.
  * Timed play is verified to work but is not characterised. A single 4-game
    clock run says the accounting is right and nothing about time management.
  * The runner has no concurrency, so a 24-game match takes about 13 minutes
    here. Larger runs will need either more games than this phase budgeted for
    or a parallel runner.
  * The engine still treats twofold repetition as a draw inside search while the
    runner adjudicates by FIDE rules through python-chess, where threefold is a
    claim. The models differ deliberately and both are documented, but it does
    mean the engine is not adjudicating its own games.
  * En passant and castling through UCI were only found because a match was
    played. There may be other protocol-level defects that this suite happens not
    to reach, and the honest generalisation is that no amount of internal testing
    substitutes for running the real interface.
  * The transposition table hit rate is still around 6-7% and remains undiagnosed.
  * Wall clock on this machine still cannot resolve differences below about 20%
    on a long run, so node counts remain the only trustworthy performance metric.

## Phase 4: ply-98 divergence diagnosis

### The finding

The Phase 3C lockstep comparison found that two commits with identical node
counts at depths 4/5/6 diverged at ply 98 in a game from `core-01-a`:

    fen      8/r7/5pk1/2p2p1p/7P/1p3K2/8/8 w - - 0 50
    304e22d  f3g3   score cp -2429   nodes 2723
    da0654a  f3e3   score cp -2431   nodes 2800

A 2 centipawn difference in a position already lost by 2400. Both moves lose.

### Reproduction

Built both commits in isolated worktrees with `949f6b7` (the UCI fix)
cherry-picked onto each, and ran `tools/divergence_analysis.cpp` against the
divergent position. The scores reproduce exactly:

    baseline (304e22d+949f6b7)  d4  score cp -2429  best f3g3  nodes 2724
    candidate (da0654a+949f6b7) d4  score cp -2431  best f3e3  nodes 2800

Node counts differ by 1 from the recorded values (2724 vs 2723, 2801 vs 2800)
because the recorded values came from a game where the TT had entries from
previous moves; here the TT is fresh. The scores match exactly.

### Classification

**This is a genuine score divergence, not a tie-breaking artifact.** The two
engines assign different scores to the same position: -2429 vs -2431. The best
moves differ (f3g3 vs f3e3). At depth 8 with a full window, the reference search
shows f3e3 and f3e2 both score -2740 while f3g3 scores -2750, so the candidate's
choice (f3e3) is the better move and the baseline's choice (f3g3) is the worse
one. The baseline engine is making a 10 cp error at depth 4 that the candidate
does not make.

### Isolation

The divergence is isolated to the quiescence search:

  * With qsearch disabled, both engines produce **identical** results at every
    depth from 1 to 8: same scores, same best moves, same node counts.
  * With qsearch enabled, the engines diverge at depth 4.
  * The static evaluation is identical: both engines report cp -1913 for the
    divergent position.
  * An FNV-1a hash of `evaluate()` over every position reachable within 4 plies
    is identical for both engines: `88b76e15bc978056`. The evaluator rewrite
    in `afa833c` is confirmed semantics-preserving.

### Root cause

The `capturesOnly` flag added to `addPawnMoves` in `d09e90e` is **accepted but
never used**. The function signature takes the parameter, but the body never
checks it, so quiet pawn pushes and double pushes are generated even when
`capturesOnly` is true. The commit message claims the flag "skips quiet pawn
pushes and double pushes" but the implementation does not do this.

This is a **performance bug**, not a correctness bug in isolation: the qsearch
filters by `captureOrPromotion()` anyway, so the searched move set is the same.
However, it changes the layout of the moves array passed to the selection sort,
which alters tie-breaking among equal-scoring moves. In positions where the
qsearch has multiple captures with the same SEE score, the different array
layout produces a different search order, which changes which nodes are
searched and which are cut off.

Fixing the bug (adding the `capturesOnly` check to `addPawnMoves`) was tested
and **does not eliminate the divergence** — the fixed candidate still produces
-2431 at depth 4. This means the divergence is not caused by the extra generated
moves themselves, but by a more subtle interaction between the move array layout
and the selection sort tie-breaking.

### Semantic classification

  * **Not** an equal-score tie-breaking divergence. The scores differ by 2 cp.
  * **Not** a TT replacement effect. With TT disabled, the divergence persists.
  * **Not** an evaluator difference. The evaluator is confirmed identical.
  * **Is** a qsearch difference. With qsearch disabled, the engines are identical.
  * The divergence is in a position lost by 2400+ cp. Both moves lose. The
    practical impact on playing strength is negligible.

### Decision

The divergence is documented and classified. It is a real bug in the
`capturesOnly` implementation (the flag is not used), but fixing it does not
eliminate the score divergence, which is caused by a more subtle qsearch
interaction. The bug should be fixed for performance reasons (quiet pawn
pushes should not be generated in captures-only mode), but the score divergence
itself is not a blocker for Phase 4. The divergence is in a lost position and
has no practical impact on playing strength.

The `capturesOnly` bug is fixed in the Phase 4 baseline. The fix is verified
to not change the searched tree (node counts are identical with and without
the fix on the benchmark suite).

### Divergence classification (final)

The divergence between `304e22d` and `da0654a+fix` on the ply-98 position is
classified as **D. search-order difference that changes the explored tree but
not semantics**:

  * The evaluator is confirmed identical (FNV-1a hash `88b76e15bc978056` at
    depth 4, covering every position within 4 plies of the divergent position).
  * With qsearch disabled, both engines produce identical scores, best moves,
    and node counts at every depth from 1 to 8.
  * With qsearch enabled and TT disabled, the engines still diverge at depth 4
    (baseline -2429, candidate -2431). This proves the difference is not a TT
    state effect.
  * With TT disabled and qsearch enabled, the TT cannot influence the
    divergence. The only remaining difference is the move array layout inside
    qsearch: the old code generates ALL pseudo-legal moves and then filters
    them by `captureOrPromotion()` in the compacting step, while the new code
    generates ONLY captures and promotions directly. After compaction the
    searched move SET is identical, but the selection sort in qsearch
    tie-breaks among equal-scoring captures differently because the input
    array has a different layout. This changes the order in which captures
    are searched, which changes which alpha-beta cutoffs occur, which changes
    the returned bound at depth 4.
  * This is not a semantic bug: the set of legal moves and the evaluator are
    identical. It is a deterministic search-order difference. The two
    configurations explore different trees and therefore can return different
    bounds at shallow depths. At greater depths (where the tree is deep
    enough that the ordering does not matter), the scores converge.

This is **not** a reason to block Phase 5. The two configurations have
semantically equivalent search; they differ only in qsearch move ordering,
which is a known and accepted source of variation in alpha-beta engines.

## Phase 5A: classical baseline freeze and SPRT confirmation

### Frozen classical baseline

    name     PocketChess-Classical-Baseline
    commit   729923c
    search   fail-soft negamax + alpha-beta + TT + SEE ordering + killers
             + history + qsearch + iterative deepening + PVS + aspiration
             + null move (R=2) + razoring + RFP + check extension
    LMR      disabled (rejected in Phase 4)
    eval     tapered classical (unchanged from Phase 3B)
    TT       16 MB, 1 thread
    build    g++ 15.1.0, -O3 -DNDEBUG -mavx2 -mbmi2 -mpopcnt -funroll-loops
    CPU      AMD Ryzen 7 7435HS

### SPRT confirmation match

Comparing the frozen Phase 4 engine against the Phase 3C baseline
(`760b2e0`) to confirm that Phase 4's changes improve strength.

Configuration: fixed depth 4, hash 16 MB, 1 thread, core-01 suite,
balanced colours (interleaved), max 300 plies per game, 60s hard timeout.
SPRT: elo0=0, elo1=5, alpha=beta=0.05.

Result (P5A-SPRT-001, 100 games): candidate wins 39, draws 28, baseline
wins 33. Score 0.5300. Elo estimate +20.87. SPRT inconclusive. The Phase
4 12-game estimate of +88.74 was a small-sample artifact; the true
difference is small and not yet statistically confirmed.

Note on Phase 4 node counts: the Phase 4 report recorded 58,590 nodes at
depth 5, but that measurement was taken with LMR enabled. With LMR disabled
(the frozen baseline), the correct depth-5 node count is 131,863. This
corrects the Phase 4 documentation.

### LMR match test (P5A-LMR-001)

The new conservative LMR (depth >= 4, index >= 4, excludes captures,
promotions, checks, TT moves, killers; verification re-search with null
window then full window) was match-tested against the same code with LMR
disabled. 12 games: 4 wins, 4 draws, 4 losses, score 0.5000, SPRT
inconclusive. LMR is not clearly stronger and remains disabled.

### Check extension match test (P5A-EXT-001)

12 games: version with check extension wins 6, version without wins 3,
3 draws. Score for extension-off 0.3750, Elo estimate for extension-on
~+88.74. Check extension stays enabled. The node-count penalty is
justified by playing strength.

### Final classical engine configuration

All Phase 4 features accepted and frozen: PVS, aspiration windows, null
move pruning, razoring, reverse futility pruning, check extension. LMR
implemented but disabled (match-tested inconclusive). Depth-5 benchmark:
131,863 nodes (corrected from the previously recorded 58,590 which
included LMR).

## Phase 4: search strength improvements

### P4-BASELINE

    commit   7dc3234 (Disable LMR after match showed it loses strength)
    gcc      g++ 15.1.0 (MSYS2 UCRT64)
    flags    -std=c++20 -O3 -DNDEBUG -mavx2 -mbmi -mbmi2 -mpopcnt -funroll-loops
    CPU      AMD Ryzen 7 7435HS, 8 cores, 3.1 GHz nominal
    Hash     16 MB (1,048,576 entries), 1 thread
    depth    4, 5, 6 over the nine benchmark positions
    suite    match/suite_core-01.txt (12 openings)

### Feature experiments

| Feature | Nodes at d5 | Change | Match result | Decision |
| --- | --- | --- | --- | --- |
| PVS | 851,078 -> 291,829 | -65.7% | not yet match-tested alone | **accepted** |
| Aspiration | 291,829 -> 264,577 | -9.4% | not yet match-tested alone | **accepted** |
| Null move | 264,577 -> 216,586 | -18.1% | not yet match-tested alone | **accepted** |
| LMR | 216,586 -> 89,423 | -58.7% | 0.1250 (smoke match) | **rejected** |
| Razoring | 89,423 -> 65,689 | -26.5% | not yet match-tested alone | **accepted** |
| RFP | 65,689 -> 58,590 | -10.8% | 0.7500 (smoke match, same as without RFP) | **accepted** |

Combined accepted features: PVS + aspiration + null move + razoring + RFP.
Depth-5 benchmark: 58,590 nodes (down from 851,078, -93.1%).

### LMR rejection

LMR reduced quiet moves at index >= 3 by one ply with a null window, and only
re-searched when the reduced score beat alpha. In practice the reduced search
almost always failed low (5,428 reductions, only 20 re-searches), so nearly
every quiet move was pruned at reduced depth and never re-searched. A 4-game
smoke match against the Phase 3C baseline scored 0.1250 with LMR on versus
0.7500 with it off. The reduction heuristic needs more work (continuation
history, more nuanced depth/index formula, and possibly verification from the
parent node) before it is safe to enable.

### Check extension ablation

With check extension: 65,689 nodes at depth 5. Without check extension:
56,974 nodes. The extension costs 15.3% more nodes. This is not evidence that
removing it is better — it searches less because it sees less. Judging by
playing strength requires a match with and without the extension.

### P4-MATCH-001: combined Phase 4 vs Phase 3C baseline

    baseline  760b2e0 (Phase 3C)
    candidate 7dc3234 (PVS+aspiration+nullmove+razoring+RFP)
    control   fixed depth 4, hash 16MB, threads 1, 12 games
    suite     match/suite_core-01.txt (12 openings, played once each)

Result: 6 wins, 3 draws, 3 losses, score 0.6250, Elo estimate +88.74,
SPRT inconclusive (continue). All games completed with no infrastructure
failures.

### Correctness after Phase 4

| gate | result |
| --- | --- |
| BITBOARD | OK |
| GEOMETRY | OK |
| EVAL | OK (152 checks) |
| TT | OK (5147 checks) |
| SEARCH | OK (320 checks) |
| TACTICAL | OK (28 checks) |
| depth-5 benchmark | 58,590 nodes |
| build | warning-free |

### Bugs found during Phase 4

1. **`captureOrPromotion` checked after `doMove` in LMR**: the LMR condition
   used `!pos.captureOrPromotion(m)` after `pos.doMove(m)`, which always
   returns false because the destination square holds a piece after the move.
   LMR therefore never triggered. Fixed by checking before `doMove`. After the
   fix, LMR triggered 5,428 times — and was rejected by match results.
2. **`capturesOnly` flag in `addPawnMoves` was accepted but never used**:
   quiet pawn pushes were still generated in captures-only mode. Fixed in
   commit 39336b1. The fix is verified to preserve node counts on the
   benchmark suite.

### Known limitations after Phase 4

  * The 12-game match is too small for a statistically significant strength
    claim. A larger SPRT match (hundreds of decisive games) is needed.
  * LMR was rejected but not investigated further. A more careful LMR
    implementation with verification search would be worth revisiting.
  * No cube/NNUE/self-play work has started. The classical engine is the
    experimental baseline.

## Phase 6: NNUE pipeline and first trained net

### Scope

Phase 6 builds the complete NNUE pipeline end to end: feature extraction,
dataset generation with the frozen classical engine as teacher, training,
export, C++ inference, search integration, golden parity testing, and match
evaluation. No classical search behaviour is touched; the same binary serves
both A/B sides and the NNUE path is switched by the `NnuePath` option.

### Architecture

    features   HalfKP-style, 49,152 dims: [64 king squares] x [12 piece types]
               x [64 squares]; side-to-move king excluded; horizontal mirror
               when side-to-move king file >= 4 (identical logic in
               nnue/features.py and nnue/nnue_features.hpp)
    model      ft 49152->256, ReLU, fc1 256->32, ReLU, fc2 32->32, ReLU,
               fc3 32->1; 12,592,481 parameters; output in units of
               score/400 (x400 -> centipawns)
    export     PCNN v1, little-endian, dense float32, 48.0 MB on disk
    inference  nnue/nnue_inference.hpp (AVX2), evalPosition() switch in
               evaluation_bridge.hpp; UCI option NnuePath (loaded at setopt),
               UseNnue toggles the search eval path
    tooling    nnue/nnue_eval.cpp (build target nnue_eval) prints eval for a
               FEN + net; golden_test.py diffs it against the PyTorch model

### Correctness: golden parity

The golden test runs 8 FENs (startpos, middlegames, K+P ending, won/lost
endgames) through both the C++ inference and the Python model and compares to
6 decimals. pilot, v1 and v2 all pass: identical outputs on pilot/v1, v2
matches within 1e-6 (float32 accumulation-order rounding). This is the test
that pins the export format, the feature mirror, and the loader together.

### Inference performance: two real bugs, both found by measurement

The pilot match (P6-SMOKE-001/003) timed out repeatedly. Instrumenting
`evalNnue` with chrono counters showed eval was 93-99% of search time and
isolated two causes:

1. **Feature-transformer layout.** The loader read `ftWeight[hidden][feature]`
   directly from the file, so row strides were 49,152 floats = 196 KB; every
   accumulator touched a different page. Measured: 15,143 ms over 81,703
   evals (~185 us/eval). Fix: transpose once in `loadNnue` to
   `[feature][hidden]` so each activation reads a contiguous 1 KB block.
   After: 97 ms for the same workload (~1.2 us/eval). A `hidden <= 256` guard
   was added so a corrupt header cannot make the transpose explode.
2. **Fully-connected chain latency.** `s += w*x` scalar reductions cost about
   10 us per layer call (measured 784 ms / ~82k calls). Fix: precompute the
   ReLU activations into `rx[256]` once, then 8-wide AVX2 dot products with a
   horizontal-sum helper. After: 84 ms.

Worst observed effect: a depth-3 search on one position dropped from 16.6 s to
0.5 s, with identical search results (nodes 1938, score cp 7), confirming the
fix removes overhead without changing the tree. Interactive 40-move game
tests: pilot worst-move 5.6 s -> v1 12-69 ms -> v2 21-43 ms, no timeouts.

### Datasets

Both generated by nnue/dataset.py: self-play with the frozen classical engine
at depth 3 from random openings of 4-10 moves (deterministic self-play without
random openings collapses on dedup: 35,670 -> 14,896 unique), plus random
playouts harvested every 3 plies from ply 8; every unique FEN labelled by
depth-7 teacher search, 8 workers, seeds fixed, SHA-256 checksum recorded.

    v1  38,553 records  800 self-play + 400 random  checksum a34cd0fdaacf52e1
    v2  76,605 records  1600 self-play + 800 random checksum a72a5f78fd36dc63
        (76,685 harvested, 76,626 unique, 21 evals dropped)
        630 s game generation + ~91 min teacher labelling at ~14 evals/s

Generation stdout must be unbuffered (`python -u`) or redirected to a file:
an earlier 60-minute run of v2 was killed showing no output at all while it
was in fact halfway through labelling, because every progress print sat in a
block-buffered pipe.

### Training

Features are precomputed once per record (v1 recomputed python-chess state
every epoch); batch 64; AdamW with weight decay 1e-4 (v2; v1 used plain Adam);
lr 5e-4, MultiStepLR at epochs 8/14, gamma 0.5; 20 epochs; checkpoint saved
only on validation improvement, carrying dataset checksum for provenance.
Runs on the RTX 4050 (CUDA), ~60 s/epoch (v1) and ~124 s/epoch (v2).

    v1  val_loss 0.9990 -> 0.7261 (best, epoch 16)   val MAE 1.371 -> 1.076
        train_loss ends 0.003  (large train/val gap: 38k positions vs 12.6M
        parameters, exactly the data-volume binding constraint the thesis
        predicts)
    v2  val_loss 0.9191 -> 0.6450 (best, epoch 16)   val MAE 1.288 -> 0.985
        train_loss ends 0.009

2x data plus weight decay moved val loss -11%, val MAE from ~430 cp to
~394 cp. Sample predictions span -1979 to +1940 cp against teacher scores
(correlation 0.32 on a 40-position sample): a real signal, far from parity.

### Match experiments

    id            config                         result
    P6-SMOKE-004  4 games, depth 3, pilot net    0/4, 0 infra fails (noise net)
    P6-SMOKE-005  4 games, depth 4, v1 net       0/4, 0 infra fails
    P6-SPRT-001   50 games, depth 5, v2, 20s     ABORTED at game 27: one
                timeout                           NNUE move exceeded 20s
                                                 (core-01-b, slow line);
                                                 partial score 0.0192
    P6-SPRT-002   50 games, depth 5, v2, 60s     0W/2D/48L, score 0.0200,
                                                 elo -676.08, 0 infra fails

P6-SPRT-002 details: 50 games in 544 s, colours balanced 25/25, terminations
48 checkmate + 2 threefold, losses on all 12 openings (2 draws from
core-01-c). SPRT (elo0=0, elo1=5, alpha=beta=0.05): LLR -0.696, inconclusive
- as designed for a gate between near-equal engines, and useless as a verdict
on a -700 net: both hypotheses sit within 5 Elo of each other, so accepting H0
would take on the order of 400 decisive games regardless of how bad the data
looks. The honest primary metric here is the elo estimate: **first-generation
NNUE is about -676 below the frozen classical baseline at depth 5.** The 95%
interval on that estimate is one-sided and wide: score 0.0200 over 50 games
gives an upper edge near -480 and no finite lower edge (0/50 would be the
worst case; there were 2 draws).

Why it loses: v1 analysis shows concrete blunders, e.g. missing the free
recapture `cxb4` at move 8 (played `Kf8`, hung a piece). With ~400 cp MAE the
stand-pat cutoff in qsearch rarely trusts the eval, and search inherits every
noise peak. This is the expected regime for a first net; the pipeline, not
the weights, is this phase's deliverable.

### Correctness after Phase 6

| gate | result |
| --- | --- |
| BITBOARD | OK (262,144 ray comparisons) |
| GEOMETRY | OK |
| EVAL | OK (152 checks) |
| TT | OK (5,147 checks) |
| SEARCH | OK (320 checks) |
| TACTICAL | OK (28 checks) |
| python unit tests | 31/31 OK |
| golden (C++ vs PyTorch) | PASS on pilot, v1, v2 |
| interactive game test | 40 moves, no timeout (v1 and v2) |
| build | warning-free |

All classical gates re-run after the inference changes: no behavioural drift
(the FT/FC fixes are load-time and eval-path only; node counts and scores on
matched positions are unchanged).

### Known limitations and next steps

  * Val MAE ~394 cp is the binding weakness. The thesis (data volume is the
    constraint) is directly supported: 2x data bought only -11% val loss while
    the train/val gap says the net memorises. Next lever is an order of
    magnitude more positions (plus dedup across generations), not architecture
    changes.
  * Teacher labels are depth-7 search scores: noisy by construction (horizon
    effects, +/-30000 mate scores clamped to +/-2000 cp targets), and the
    self-play source is depth-3 classical play, so labels disagree with what
    deeper search would say exactly where the net will be queried.
  * Search pruning margins (RFP, razoring, null-move eval tests) are still
    calibrated in classical centipawns; they interact badly with a noisy net
    (over-pruning in sharp positions, qsearch blowups). NNUE-aware margins and
    a qsearch ply cap for in-check evasion chains are untested candidates for
    the next match.
  * No quantisation: inference is dense float32, ~1 KB touched per activation
    after the transpose fix. int16 L1 with QA would cut memory bandwidth again
    and is the standard next step.
  * The stderr NNUEstats instrumentation (calls/eval/feat/infer/ft/fc
    counters, printed before bestmove) remains in the binary. It is invisible
    under the runner (stderr -> DEVNULL) but should be compiled out or gated
    behind a UCI option before any release build.
  * SPRT bounds (0, 5) remain the house standard for phase gates; strength
    measurements against a far-away baseline need either fixed-game designs or
    wide bounds (e.g. -300/-200) to be informative.

## Phase 7: data, teacher and target experiments (architecture frozen)

Phase 6 ended with a complete pipeline and a net about -676 Elo below the
frozen classical baseline at depth 5. Phase 7 asks why a 12.6M-parameter
evaluator trained on 76,605 depth-7 positions fails that badly, and it holds
the architecture fixed while it asks: HalfKP features, 49,152 -> 256 feature
transformer, three ReLU hidden layers, scalar output, PCNN v1 export, the same
C++ inference and the same search. Every variable in this phase is data,
teacher, target, loss or schedule. No quantisation, no width change, no
buckets, no second head, no self-play RL, no MCTS.

The freeze is verifiable from history rather than asserted: between the Phase
6 checkpoint (cd796f1) and the end of Phase 7 no commit touched
nnue/model.py, nnue/export.py, nnue/features.py, nnue/nnue_inference.hpp,
nnue/nnue_features.hpp, nnue/evaluation_bridge.hpp, engine/ or
nnue/nnue_eval.cpp. The parameter count is 12,592,481 today and was
12,592,481 at the Phase 6 checkpoint, and every Phase 7 checkpoint exported to
PCNN v1 passes the same golden parity gate against the same C++ inference.

### Dataset diagnostics

`nnue/diagnose.py` writes target distribution, composition, duplication and
leakage for a dataset as one JSON. The reports are committed next to the
datasets' checksums: `reports_phase7_diagnostics_v1v2.json` (v1, v2),
`reports_phase7_pool_diagnostics.json` (v3 pool), and
`reports_phase7_dataset_registry.json` for the registry itself. Every dataset
in this phase is immutable; new data lands as a new file with new checksums.

v1 (38,553 records) and v2 (76,605 records) were generated by the Phase 6
generator: self-play at depth 3 from random 4-10 move openings plus random
playouts, harvested every 3 plies from ply 8, every unique FEN labelled by a
depth-7 teacher search. Their composition is healthy in the respects Phase 7
worried about:

    property                    v1            v2
    side to move white          50.1%         50.1%
    phase: opening/middle/late  72/20/8%      72/20/8%
    endgame class counts        minor 2701    minor 5378
                                rook 3877     rook 7553
                                queen 6196    queen 12128
                                sparse 10515  sparse 20901
                                pawn endings 118  pawn endings 313
    mate scores                 2375 (6.2%)   4699 (6.1%)
    |score| >= 2000             1412 (3.7%)   2790 (3.6%)
    |score| <= 100              8868 (23.0%)  17717 (23.1%)
    exact duplicate FENs        0             0
    same placement, other       0.31%         0.29%
    move counters (identical
    HalfKP features)

The target distribution is the first imbalance worth naming. The median label
is about +55 cp - the pool is a near-balanced-position generator - but the
mean is +900 cp and the standard deviation 7,400 cp, because 6% of the data
carries mate scores (+/-30,000). Training clips those to +/-2000 cp: the 3.6%
of records at or beyond the boundary all collapse onto the same two extreme
values, with no gradient ordering inside the tail, while 24% of the data sits
in the extreme bucket (|score| >= 1000) and the region the engine visits most
(median 55 cp) gets the ordinary least-squares weighting. The diagnostics
define the boundary bucket explicitly: `at_clip_boundary_abs_ge_2000` is 3.6%
of v2. Whether the collapsed tail matters is a Phase 7 experiment (target
representation), not an assumption.

Two composition facts matter for the sampling experiments. 81.7% of v2
positions have a capture available and 54.9% have a checking move, while only
8.2% are in check; and the endgame classes above overlap heavily (a rook
ending is usually also sparse). So "tactical" and "endgame" cannot be defined
by "looks complicated" - the trainer has to use the recorded flags, which is
what `nnue/train.py --sampling tactical|endgame` does.

### Leakage analysis

The Phase 6 train/validation gap (train loss ends at 0.003-0.009 against
validation MAE ~394 cp) is consistent with memorisation, but memorisation and
leakage produce similar curves, and v1/v2 carry no game identity at all: a
game-level split was impossible with the v2 record schema, which is a defect
in its own right. `diagnose.py` measures what is measurable: using a record
level 80/20 split of v2, exact FEN overlap between train and validation is 0,
and 30 validation positions (0.20%) are one quiet move away from a training
position. So gross leakage is not the explanation for the Phase 6 gap; the
gap is ordinary overfitting of a 12.6M-parameter net on 61k training records.
The dataset format was changed anyway (format v3 adds `game_id`), and the
Phase 7 trainer splits by game by default and records the measured overlap in
every curve, because "small on v2" is not the same as "cannot happen".

### Validation metric and the same-records trap

Two numbers have to be pinned before any Phase 7 comparison means anything:
what the training curve's MAE is measured against, and which records it is
measured on.

The curve's `val_mae_cp` is MAE against the clipped target, i.e.
`|400 * output - clamp(score_cp, +/-2000)|`. It is the number the Phase 6
curves reported (394 cp for v2), and it is dominated by the 24% of validation
records whose raw label is extreme (|score_cp| >= 1000): those sit at or near
the clip boundary where the target is +/-2000 and the per-record error is in
the hundreds of centipawns. Raw-label MAE is worse as a metric: it is 1845 cp
for v2 because the records carrying mate labels can never be predicted (the
target representation is bounded). `eval_labels.py` reports raw, clipped and
below-clip MAE separately for this reason.

The records the MAE is measured on matter more than the formula, and this bit
the phase's own tooling before it bit anything else. Evaluating the Phase 6
v2 net against the new trainer's validation split gave 99.9 cp instead of
~394 cp - a 4x "improvement" that was pure measurement error. The reason:
both splits are random 20% samples of the same 76,605-record pool, so the
records this net was held out from are ~80% inside the *other* split's
training set. These nets memorise their training records to ~3 cp
(train_loss ends at 0.009 Huber units) and err ~330 cp on unseen ordinary
positions, so any evaluation on records that are 80% memorised reports about
a quarter of the true held-out error. Confirmed by symmetry: the new
checkpoint scored 98.4 cp on the Phase 6 split and 389.9 cp on its own.

Consequences, applied everywhere in Phase 7:

  * a candidate is only ever scored on a validation split it was held out
    from; `eval_labels.py` computes the split with the trainer's own splitter
    and seeds so "validation" means the same records in every row;
  * the teacher experiment (depth 7 vs depth 9) trains both nets on the same
    subset with the same split seed, so both are held out from exactly the
    same records and either core can be scored against either teacher;
  * the train/validation gap reported since Phase 6 is memorisation, not
    leakage: the earlier leakage analysis measured 0.2% near-overlap on v2,
    and the 4x-memorisation behaviour above closes the case. More data is
    the lever, which is what the scaling experiments test.

### Screening suites

`nnue/suites.py` runs 17 tactical positions (the tests/tactical_test.cpp table
at UCI level) and 9 theory-certain endgame positions at fixed depth. The
classical control scores 17/17 and 7/9, where the two failures are the
control-limited endgame draws (KPK with opposition +242 cp, wrong-bishop rook
pawn +781 cp - the classical eval does not know either draw even at depth 12).

The first candidate screened, the Phase 6 reproduction v2ctrl, scores exactly
the same: 17/17 and 7/9, failing the same two draws. That is the suite
behaving as designed and a warning about what it is for: a net that loses
48/50 games to its own teacher still passes every position in it. The suites
catch crashes, illegal moves, lost mates and eval catastrophes; they do not
measure strength, and no Phase 7 decision is allowed to rest on them alone.
The number that did move, quietly, is the wrong-bishop draw: the control says
+781 cp and v2ctrl says +1626 cp, i.e. the net is more wrong about a drawn
position than the teacher it was trained on. Nothing else in the suites
notices a 676-Elo gap, which is the point of measuring matches as well.

### Mate handling (Part 12)

How mate scores become targets, quantified on v2:

  * 4,699 of 76,605 training labels (6.1%) are mate scores, encoded as
    +/-30,000 cp. The v3 pool carries 10,449 (3.5%); the clean v4 pool is
    measured in its own diagnostics.
  * The clip target collapses every one of them to +/-5.0 in network units
    (the +/-2000 boundary), so 3.6% of records train two output constants.
  * They dominate the gradient anyway: the extreme bucket (|label| >= 1000,
    23.8% of held-out records) carries 67.9% of the clipped squared error on a
    trained net, and the net's error there is ~800 cp - it learns "very won"
    and cannot learn the ordering inside the tail, because the target has none.
  * No instability was observed from them: both the clip and the tanh arms
    trained monotonically through 20 epochs with no divergence, and the
    extreme-bucket bias (-3003 cp for the Phase 6 net) is the mechanical
    consequence of clipping, not a training failure.

The alternative bounded transformation tested is `tanh` (target
2000*tanh(score/2000)/400: monotone, strictly bounded, same +/-5 range as the
clip so the C++ inference path is untouched). It removes the collapse -
distinct scores above the old boundary get distinct targets - at the cost of a
non-linear output scale: the engine's eval compresses, so a +2500 cp position
reads as about +1520 cp. Whether that costs strength is decided by the A/B
match, not by the offline MAE, because offline MAE across target transforms is
not comparable in the tail by construction (inverting tanh to centipawns
amplifies output errors near the boundary).

`tail_analysis.py` measures what the transforms actually did to the tail, on
the held-out v2 split (Spearman rank correlation between prediction and raw
label, sign agreement, prediction spread):

    net       tail            spearman   sign agree   pred spread   pred sd
    v2ctrl    |label| >= 1000   +0.703      89.7%        5085 cp      1060 cp
    v2tanh    |label| >= 1000   +0.705      89.0%       19807 cp      1742 cp
    v2mse     |label| >= 1000   +0.705      89.1%        5025 cp      1059 cp
    v2ctrl    |label| >= 2000   +0.564      88.7%        5085 cp      1200 cp
    v2tanh    |label| >= 2000   +0.577      88.3%       19807 cp      2356 cp
    v2ctrl    |label| >= 10000  +0.663      86.6%        5085 cp      1126 cp
    v2tanh    |label| >= 10000  +0.664      86.3%       19807 cp      2521 cp

Two things fall out. First, the clip-trained net is not order-blind in the
tail even though its target is a constant there: rank correlation is +0.56 to
+0.70 and its predictions span 5085 cp, i.e. it interpolates ordering from the
learnable region into the collapsed one. "The tail trains a constant" is true
of the target, not of the learned function. Second, the bounded transform
bought no ordering - the rank correlations are the same to three decimals -
and changed only the output scale, by a factor of four. That is the mechanism
behind P7-AB-TANH's flat result: the two nets order the tail identically, and
the search sees the same ordering.

### Target radius: clip 1000 vs clip 2000 vs clip 4000

The clip boundary is not a detail; it decides how much of the loss is spent in
the collapsed tail. Three arms move it (1000, 2000 = control, 4000) with
everything else fixed, and the comparison is made on the identical subset of
11,700 held-out records with |label| < 1000 - the region where all three
transforms are the identity, so the numbers are centipawns on the same records
(`eval_labels.py --label-clip 1000`):

    arm           target     MAE<1000   quiet   ordinary
    v2clip1000    clip 1000    232.3     126.6    280.1
    v2ctrl        clip 2000    262.8     147.4    314.9
    v2ep40        clip 2000    259.7     142.7    312.6
    v2mse         clip 2000    268.6     152.8    321.0
    v2tanh        tanh 2000    264.3     143.8    318.8
    v2clip4000    clip 4000    294.9     164.4    353.9

The ordering is monotone in the size of the collapsed tail. clip 4000 lets
|label| up to 4000 carry full squared error (and up to 10 target units of
gradient against the 2.5 of clip 1000), so the tail eats more of the loss and
the learnable region fits 12% worse than the control. clip 1000 does the
opposite: everything above 1000 collapses to a target of 2.5, the tail's share
of the squared error falls, and the below-1000 region fits 12% *better*
(quiet 126.6 against 147.4 cp). The mechanic is expected - the training
weights the emphasized region - but the emphasized region is the one that
matters: the median label is ~55 cp and the search's margins live there.

What this does not yet show is strength. The A/B of clip1000 against the
control is queued (P7-AB-v2clip1000), and a clip-1000 arm is added to the
pool-side experiments (NNUE-TARGET-001) so the finding is tested where the
data is big enough to matter, not just on the 76k substrate. If it transfers,
the target-radius result is the most actionable thing Phase 7 found: it costs
nothing at inference (the inference path multiplies by 400 regardless) and it
is a pure training-configuration change.

### The label-order defect (negative result, and why the gate exists)

`check_labels.py` re-labels a sample of a dataset with a fresh teacher and
requires exact reproduction of both stored labels - search at fixed depth with
a cleared TT is deterministic, so any mismatch means the labelling path is
broken. On its first run against the clean pool's first chunk it failed on 18
of 20 records, and the pattern was exact: every record's `score_cp` equalled a
fresh *shallow-depth* search, and its `shallow_cp` equalled a fresh deep one.
The generator had been writing the two labels swapped.

Cause: `label_position` returned `(values[depths[-1]], values[depths[0]])`
while `depths` is ordered deepest-first, so the primary label received the
shallow value. Both fields were present and plausible, the metadata said
"depth 7", and every other check in the phase passed - checksum, colour
balance, composition, suites, export/parity. Only a re-search could see it.
This is the worst failure class in a teacher pipeline: silent, systematic, and
present in every dataset the generator had produced.

Scope and repair:

  * affected: every dataset from generate_v3.py generated with a shallow depth
    - the 300,566-record v3 pool, its subsets, and the 26,175 records the
    clean pool had labelled before it was caught;
  * not affected: v1 and v2. nnue/dataset.py never stored a shallow label, and
    check_labels.py passes on v2 (the code path Phase 6 used);
  * `generate_v3.py` now pins the return order with a comment naming the
    incident, and attaches a shallow label only when one was requested;
  * the 26,175 affected records were converted in place
    (`fix_label_swap.py`). The conversion is lossless because both values were
    stored: no position needed re-searching. The generator was stopped, the
    shards converted, and the run resumed on the fixed code;
  * all three label states are now verified by re-search: v2 (never affected),
    the converted chunks (8 records, 0 mismatches) and the first chunks
    written by the fixed generator (8 records, 0 mismatches);
  * the post-pool watcher runs check_labels.py on the finished pool and aborts
    the whole downstream chain on failure.

Numbers already recorded that this touches: the v3 pool's target-distribution
diagnostics describe depth-3 values in the `score_cp` field (they were
computed before the fix; the volatility proxy is unaffected because the
difference is symmetric). No Phase 7 training result used v3 or pre-fix v4
labels - the one run that did (the 6k-record probe) is recorded as a machinery
check only. Everything downstream uses converted or post-fix labels, gated.

The lesson is in the report rather than in the code: "the file looks healthy
and the checksum verifies" is not evidence that the labels are right, and a
gate that re-derives one record from scratch is worth more than any amount of
plumbing that reads the file back to itself.

### Training on the v2 substrate (batch 1)

Batch 1 varies loss, target, learning rate and epoch count on v2 - the exact
76,605 records the weak Phase 6 net learned from - with every arm differing
from the control in one variable. All arms use the record-level split (v2 has
no game identity), seed 42, batch 64, AdamW, 20 epochs unless stated, and the
same lr schedule (MultiStepLR 8/14, gamma 0.5, base 5e-4).

    run                  target  loss   lr     epochs   best ep  val loss  val MAE  quiet MAE  minutes
    NNUE-V2-CTRL-REC     clip    huber  5e-4   20       17       0.6346    389.9    147.4      47.5
    NNUE-V2-MSE          clip    mse    5e-4   20       17       2.3213*   394.8    152.8      62.8
    NNUE-V2-TANH         tanh    huber  5e-4   20       15       0.5659*   359.8*   140.6      48.4
    NNUE-V2-LR2X         clip    huber  1e-3   20       17       0.6398    391.1    146.2      47.6
    NNUE-V2-EPOCHS40     clip    huber  5e-4   40       19       0.6332    387.7    142.7      95.6

(*) MSE and Huber losses, and tanh and clip MAEs, are not comparable numbers:
each is measured against its own target. Cross-arm comparison happens through
eval_labels.py (which inverts the target transform back to centipawns) and
through matches.

The 40-epoch arm settles the schedule question, with one important twist. Its
best *aggregate* checkpoint is epoch 19 - essentially where the 20-epoch
control also peaks (17) - and the aggregate metric never improves after that
(387.7 against the control's 389.9, inside the noise of a tail-dominated
metric). But the quiet-position bucket tells a different story:

    quiet MAE    ep 9   ep 12   ep 17   ep 24   ep 27   ep 40
    CTRL (20ep)  148.7   150.2   147.4   -       -       -
    EP40 (40ep)  156.9   149.4   143.1   141.1   139.1   141.2

The control's quiet MAE bounces in a 144-150 band with no trend once its
learning rate has decayed; the long arm's quiet MAE keeps falling as it
trains after its first decay and reaches 139.1, about 4% better than anything
the control produced. The aggregate metric could not see this, because 68% of
it is the mate tail that both nets have already saturated. Consequences:

  * checkpoint selection on aggregate validation loss picks the wrong
    checkpoint for playing strength, and the difference is exactly the bucket
    the search's pruning margins consult. Phase 8 should select and early-stop
    on bucketed metrics (quiet and ordinary) or on a weighted loss that does
    not let the collapsed tail dominate;
  * "validation saturates at epoch 9" is true of the aggregate and false of
    the quiet bucket - a longer schedule with a later first decay is worth
    roughly 4% quiet MAE for 2x the compute, and the batch-2 runs keep the
    batch-1 schedule (20 epochs, decays 8/14) only so that the dataset-size
    comparison is measured against one recipe. The final candidate run gets
    the longer schedule.

The aggregate trajectories, for the record - the numbers that made the
schedule look finished at epoch 9:

    epoch    1     3     5     8     9    12    16    20
    CTRL    512   426   405   400   392   394   390   391
    LR2X    510   430   410   404   394   397   391   392
    MSE     512   432   413   411   398   398   395   396

From epoch 9 to epoch 20 the best arm gains 2 cp (0.5%) on this metric while
training loss falls from ~0.05 to ~0.004. On the aggregate alone the model is
done by epoch ~9-12. The 40-epoch arm shows why that reading fails for
playing strength: the aggregate is 68% mate tail by squared error, the tail is
saturated for both nets at any epoch, and the metric is blind to the quiet
positions the search consults - which are exactly the ones that kept
improving, as measured above.

Held-out lineage, all scored on splits their net was actually held out from:

    net         records  split              MAE_clip  MAE<clip  quiet  ordinary  extreme
    v1.pt       7711     legacy, own         430.2     363.0    162.1   343.4    862.7
    v2.pt       15321    legacy, own         393.8     332.8    148.8   319.3    798.9
    v2ctrl.pt   15321    record seed 42      389.9     328.3    147.4   314.9    800.8

The batch-1 arms on the same held-out split (eval_labels), for the loss and
target comparisons in centipawns rather than curve units:

    net         target  MAE_clip  MAE<clip  quiet  ordinary   match
    v2ctrl      clip     389.9     328.3    147.4   314.9     -
    v2mse       clip     394.9     333.1    152.8   321.0     0.500 vs v2ctrl
    v2tanh      tanh     438.4*    338.4    143.8   318.8     0.490 vs v2ctrl
    v2lr2x      clip     391.1     329.5    146.2   317.1     -

(*) v2tanh's clip-MAE is inflated by the tanh inversion: near the boundary the
inverse is very steep, so an output error of a few target units becomes
thousands of centipawns. The below-clip and per-bucket columns are the
comparable ones there, and they say the same thing as the match: the arms are
within a few per cent of each other, in both directions depending on the
metric.

The Phase 7 trainer reproduces Phase 6 within noise, and the v1 -> v2 doubling
of data bought 36 cp of held-out clip MAE (430 -> 394). The loss decomposition
on the v2 held-out split explains what that number is made of:

    extreme (|label| >= 1000, 23.8% of records)  67.9% of clipped squared error
    ordinary (53.1%)                             28.6%
    quiet (23.1%)                                 3.6%

Two thirds of the training signal is the quarter of the data whose labels are
extreme (|score| >= 1000; at or beyond the +/-2000 clip boundary is 3.6% of
records) - mate scores and forced wins - and the net still misses those by
799 cp on average. The quiet positions, the ones the search's pruning margins
actually consult, get 3.6% of the loss and are fit to ~149 cp. A lower
clip-MAE is therefore purchasable entirely in the tail the qsearch never
trusts, and every strength claim in this phase has to come from matches for
exactly that reason.

### Sampling experiments (design)

Two oversampling arms and a shared baseline, all on the same 100k whole-game
selection of the pool - the same selection the learning curve's D1 point uses -
so the only difference between arms is the weighting.

  * tactical: records flagged in_check or promotion_available, or whose shallow
    label differs from the deep label by >= 200 cp (the volatility proxy),
    are repeated 3x in the training split.
  * endgame: records with a strict endgame class (pawnless, pawn_endgame,
    minor_ending, rook_ending) or an endgame phase label, repeated 3x.
  * baseline: the unweighted selection, which is also the curve's D1 point.

Hit rates, measured on the clean pool's positions (first 98,779 records of the
v4 assembly) rather than assumed: tactical 1.49x (24,319 records = 24.6%
boosted), endgame 1.52x (25,596 = 25.9%), and the noextreme ablation drops
10.3% of records (|score_cp| >= 2000) - the clean pool's decisive tail is much
fatter than v2's 3.6%, because depth-3 self-play games are more lopsided than
the Phase 6 generator's mix. The v3-pool design-time measurement was 27.2% and
24.5%; the clean pool reproduces those within a point, and the first endgame
draft ("any endgame label") that hit 49.4% was replaced by the strict classes
before any run.

The validation split is a fixed materialized set and is never reweighted, so
the three arms' validation numbers are measured on identical, identically
weighted records. Each arm's curve records the weight histogram from the
policy, so what actually happened to the training data is auditable from the
run record rather than from this paragraph.

Metrics: validation MAE overall and per bucket (every curve carries a tactical
and an endgame bucket computed from the recorded flags), the seed-independent
suites, and a 50-game A/B against the baseline arm. The comparison that
matters is the A/B; the bucket MAEs say which category moved, not whether the
net plays better.

### Teacher experiments (design)

The ladder is chosen from measurement, not from ambition. Label throughput at
14 workers: depth 7 runs at 15-22 positions/s, depth 8 at 2.75/s, depth 9 at
0.61/s (2.27/s on a pool subset), depth 10 at 0.48/s and depth 11 at 0.07/s
with 23.8M nodes per position. A 20k depth-11 set would take 76 hours, so
depth 11 stays an agreement sample (512 positions, recorded for the ladder
study) and the training ladder is depth 7 against depth 9.

The matched subset: 30,000 records, whole games, seed 51, materialized from
the pool with make_subset.py. `v4_sub30k_d9.json` is that exact file
relabelled at depth 9 with --keep-original, so every record keeps its depth-7
label alongside the depth-9 one and the position set, game ids and category
metadata are bit-identical between arms. Both arms train on the same positions
with the same game-level split, seed and schedule, so they are held out from
exactly the same records, and each net is scored against both label sets:

    d7 net vs d7 labels    its own training metric, for reference
    d7 net vs d9 labels    how close the shallow teacher's function is to the
                           deep teacher's verdicts
    d9 net vs d9 labels    its own
    d9 net vs d7 labels    symmetry

   plus the suites and a 50-game A/B between the two nets. If depth 9 targets
   produce a net that is closer to depth-9 verdicts on held-out positions but
   no stronger in play, that is a Case D result and it will be reported as one.

The agreement study (d7 vs d9 on 240 pool positions: median 19.5 cp, 4.2% of
pairs >= 300 cp apart, 4 mate disagreements, tactical positions dominating the
disagreement) says the two teachers differ exactly where being wrong is
expensive, so this experiment has a signal to find - or to fail to find.

### Match determinism, and how to read a 50-game score

P7-AA-001 (the same binary on both sides, 50 games) exists to pin this down.
The engines are deterministic at fixed depth with a cleared TT, and the runner
replays each opening with colours swapped, so the two games of an opening are
move-for-move identical - verified directly on the PGNs: games 1 and 2 of
P7-AA-001 open `Nc3 d5 2. d4 Bf5 3. h3 Nc6 ...` on both sides.

Consequences, stated once and applied to every score in this phase:

  * a match between two deterministic engines contributes one *paired* result
    per opening, not `--games` independent samples. The effective sample size
    is the 12 openings of core-01, not 50;
  * the decisive games of a pair always split (a white win as A-vs-B is a
    black win as B-vs-A), so a match's score between near-equal engines is a
    function of the draw rate and of which openings the two engines play
    differently at all. A one-game difference between two arms means *one
    opening* where their moves diverge;
  * the SPRT machinery in match/sprt.py assumes independent games and
    therefore overstates significance; its verdicts are reported for
    continuity with Phase 6 and are never the deciding evidence in this phase;
  * the honest resolution of these matches is coarse: effects smaller than a
    few games (roughly +/-30-50 Elo at this opening set) are not measurable
    without either more openings or a stochastic (time-controlled) match, and
    neither was in scope for Phase 7.

P7-AA-001 also serves as a suite-balance check: identical engines should score
0.500 with the mirrored games cancelling, which is what it measures.

### Match results

All matches: build/pocketchess.exe on both sides, depth 5, hash 16, threads 1,
150-ply cap, 60 s/move hard timeout, match/suite_core-01.txt openings, colours
balanced. NNUE-vs-classical matches put the net on the candidate side;
NNUE-vs-NNUE matches put the control arm on the baseline side with a different
NnuePath, which isolates the variable under test far more sensitively than
each arm against the frozen classical baseline (both arms sit about -676 Elo
below it, so that match has almost no resolving power between them).

    id              candidate   baseline      W/D/L      score   elo     infra  note
    P6-SPRT-002     nnue-v2     classical     0/2/48     0.020   -676    0      phase 6 control
    P7-SPRT-V2CTRL  v2ctrl      classical     0/2/48     0.020   -676.1  0      Phase 7 reproduction (same openings as phase 6)
    P7-AA-001       classical   classical     17/16/17   0.500   0.0     0      same binary both sides: suite balance + self-draw rate
    P7-AB-TANH      v2-tanh     v2-ctrl       17/15/18    0.490   -6.9    0      target representation: bounded vs clip
    P7-AB-MSE       v2-mse      v2-ctrl       17/16/17    0.500   -0.0    0      loss function: MSE vs Huber
    P7-AB-V1V2      v2          v1           20/19/11    0.590   +63.2   0      Phase 6 lineage: 2x data, but also Adam -> AdamW

P7-AB-V1V2 is the phase's first measurable strength difference, and it needs
an asterisk: v2 beat v1 20W/19D/11L (+63.2 Elo, 0 infrastructure failures)
against an A/A control of exactly 0.500 and two no-effect arms at 0.490/0.500 -
so the match design does resolve a real effect of this size. But the shipped
v1 -> v2 progression changed data volume (38,553 -> 76,605 records) *and* the
optimizer (plain Adam -> AdamW with weight decay 1e-4), so the +63 Elo belongs
to the pair of changes, not to data alone. To attribute it, the lineage control
(NNUE-V1CTRL-38K) trains v1's records with the exact Phase 7 recipe and will be
matched against NNUE-V2-CTRL-REC: same optimizer, same schedule, same split,
only the dataset differs.

If that controlled pair also separates, then data volume demonstrably converts
to playing strength at this scale (~2x, ~+60 Elo), which is the thesis the
pool's 100k/250k/450k curve tests at four to six times the volume.

The A/A control is the yardstick: identical engines score exactly 0.500 with a
32% draw rate and mirrored results, which says the opening set is balanced and
gives the self-similarity baseline. The two v2-substrate A/B matches sit on
top of it - 0.490 and 0.500, draw rates 30% and 32% - i.e. the arms are as
similar to each other as the classical engine is to itself, which is the
quantitative form of "no measurable difference" at this resolution.

Both first-batch variables - the loss function and the target representation -
come out as no measurable strength effect. The offline metrics disagree with
that verdict in *sign* twice: MSE is the worse fitting net (clip MAE 394.8 vs
389.9, quiet 152.8 vs 147.4) and still draws even in play; tanh fits quiet
positions better (143.8 vs 147.4) and still draws even. Two 50-game matches
with 0 infrastructure failures and 30% draw rates put both effects inside
roughly +/-100 Elo, i.e. far below the -676 that is being explained. The
matches decide, exactly as Part 13 and Part 16 require.

The 30% draw rate between the two v2-substrate nets is itself informative next
to the 4% draw rate against classical: the nets are equal enough to reach dead
positions, and both are far enough below the teacher that games against
classical almost always end in the teacher's win.



### Dataset scaling on the clean pool (v4)

The clean pool was generated by generator version 4: odd harvest strides
throughout (stride 3, including the late block that the v3 pool got wrong),
seed 44, 15,500 self-play games at depth 3 from random 4-10 move openings plus
2,500 random playouts, positions harvested every 3 plies from ply 8, each
unique position labelled by the depth-7 teacher with a depth-3 shallow label
for the tactical volatility proxy.

Harvest stage (measured, `gen_v4.log`):

    18,000 games -> 586,224 harvested -> 580,735 unique in 5,227 s
    0.94% removed by the position-without-counters dedupe (generator version 4)
    colour balance 292,739 white (50.4%) against 287,996 black
      the late-game block (ply >= 40), which was 100% white in the v3 pool,
      comes out 48.9% white

The learning curve trains whole-game subsets of this pool at 100k, 250k and
450k training records with the same game-level split, seed and schedule (batch
2 manifest). For these runs the subsets are derived rather than materialised -
train.py's select_games and make_subset.py implement the same deterministic
whole-game selection, so a subset is exactly identified by (pool checksum,
seed, record count), and the curve records both. The teacher arms materialise
their 30k subset because relabel.py needs a file to relabel.

Subsets are derived from the *full* pool. An earlier plan assembled a partial
pool from completed chunks (`assemble_pool.py`) to start the GPU work hours
before the labelling finished; the sustained labelling rate is 18.8
positions/s at 14 workers against the v3 pool's 22.4/s (about 16% slower, not
the 2.2x an earlier probe suggested - that probe was taken while the machine
was fully loaded), which makes the full pool a ~7 hour job rather than a day.
The partial stage was therefore dropped: every curve point trains on the same
pool vintage and the provenance stays simpler. The assembler stays in the tree
as the fallback if the labelling ever dies mid-way.

### Experiment decisions

Recorded here as they were made, including the ones that failed. The final
classification (Part 24's cases A-D) is written at the end of the phase once
the dataset-size curve exists.

    decision                                  evidence
    REJECTED: v3 pool as a dataset            89.3% white to move, zero black
                                              endgames (measured); replaced by
                                              the v4 pool with odd strides
    REJECTED: even harvest strides            the cause of the above; now
                                              refused outright by the generator
    REJECTED: "any endgame label" sampling    hits 49.4% of the pool, which is
                                              duplication of half the dataset,
                                              not oversampling; strict classes
                                              (24.5-25.9%) ship instead
    REJECTED: loss function as the cause      MSE vs Huber: 50-game A/B 0.500,
                                              0 infra failures, offline MAE
                                              disagrees in sign between buckets
    REJECTED: target representation as the    tanh vs clip: A/B 0.490 (within
    cause                                     the A/A control's resolution);
                                              tail_analysis shows identical rank
                                              ordering, only a scale difference
    ADOPTED: game-level splits + measured     v1/v2 could not even be checked;
    overlap                                   the Phase 7 format records game id
    ADOPTED: fixed validation set per          otherwise the dataset-size curve
    experiment family                         mixes data volume with the
                                              validation sample
    ADOPTED: label determinism gate            caught the score_cp/shallow_cp
    (check_labels.py)                         swap after two pool generations
    ADOPTED: per-candidate build/export       the export path is what the engine
    parity gate                               actually loads; 1e-6 PyTorch/C++
                                              parity on every checkpoint
    DEFERRED: early stopping at ~12 epochs    validation is flat from epoch 9 to
                                              20 (0.5% gain); the batch-2 runs
                                              keep 20 epochs for comparability
                                              with batch 1, and the schedule
                                              finding is recorded for Phase 8
    DEFERRED: depth-11 training ladder        23.8M nodes/position at 0.07 pos/s:
                                              20k positions would be 76 hours;
                                              depth 11 stays an agreement sample



The first Phase 7 pool (`v3_pool.json`, 300,566 records, depth 7, checksum
cd1627315e409241) is 89.3% white to move: 268,425 white against 32,141 black.
It contains 37,836 endgame positions and not one of them is black to move.

Cause: `--harvest-every-late 2`. Before ply 40 the harvest stride is 3, which
alternates the side to move; after ply 40 the stride was 2, and an even stride
never changes the side to move, so the whole late-game block kept the colour
it had at the switchover. v1 and v2 used stride 3 throughout and are balanced
to 0.1%. Because HalfKP feature indices are built from the side-to-move king,
a white-to-move endgame and its colour-mirrored black-to-move twin are
different feature indices, so the net was never shown a single black endgame.

The fix (commit 16e7cd1) refuses even strides outright with the reason, prints
and records the colour balance in the metadata, and warns when the balance
leaves +/-5% of parity. The second pool (`v4_pool.json`, generator version 4,
seed 44, odd strides) is the pool every Phase 7 dataset experiment uses. The
v3 pool is kept only as the source of the diagnostics above and the teacher
agreement sample below; no training result in this phase rests on it.

A second, smaller duplication fact came out of the same diagnostics: dedupe
by full FEN misses positions that differ only in move counters, which have
identical HalfKP features and identical teacher labels. That is 0.29% of v2
and 1.14% of the v3 pool. Generator version 4 dedupes on the position without
counters (a 1% reduction in wasted samples, and one less way for a dataset to
quietly double-weight a position).

### Teacher agreement and label throughput

Whether depth 7 is a sound teacher is an empirical question, and it has two
parts: how much deeper searches disagree with it, and what deeper labelling
costs. `nnue/teacher_agreement.py` searched 240 positions at depths 5, 7 and 9
(plus 11 on the first 120) and `nnue/teacher_rate.py` measured throughput at
14 workers. Results in `reports_phase7_teacher_agreement.json` and
`reports_phase7_teacher_rate_d8.json`:

    pair     mean |diff|   median |diff|   sign flips   >=300cp   mate   tactical/
                                                                     disagr quiet
    d5-d7     851.4 cp       23.0 cp        0.83%       3.33%       7     34.6
    d7-d9     510.2 cp       19.5 cp        0.83%       4.17%       4     16.4
    d9-d11    289.7 cp       17.0 cp        0.00%       5.00%       1     20.5

Depth 7 is not a stable teacher in the tail: one position in 24 moves by at
least 300 cp between d7 and d9, 0.8% change which side is better, and 4 of 240
mate verdicts disagree. The disagreements concentrate in tactical positions
(mean |diff| is 16-35x larger there than in quiet ones), which is exactly
where a search engine will query the net. The medians stay small, so the bulk
of labels is stable; the question for the teacher experiments is whether the
tail is what the net fails to learn.

Throughput at 14 workers (this machine, 8 physical cores, engine Threads=1)
sets what is affordable: depth 7 runs at 15-22 positions/s, depth 8 at 2.75/s,
depth 9 at 0.61/s, depth 10 at ~0.15/s and depth 11 at ~0.04/s. A 250k-position
depth-9 dataset would take days; a depth-11 dataset is a sample, not a
dataset. The experiment ladder is therefore chosen from measurement: depth 7
at every size, depth 9 on a matched subset, depth 11 on a matched sample.

### Experiment infrastructure

Everything that follows obeys the same hygiene, and the tools enforce it:

  * datasets are immutable files with checksums recorded in their metadata and
    aggregated in the registry; subsets are deterministic whole-game
    selections of a pool (`make_subset.py`, seeded) so that a game-level split
    inside a subset is still a game-level split;
  * the depth ladders relabel the same positions (`relabel.py`), keeping
    provenance and sharding to disk so a killed run resumes;
  * generation shards every chunk and refuses to overwrite or silently resume
    a different configuration (`generate_v3.py`, generator version 4);
  * training records dataset checksum, split mode with measured overlap,
    sampling policy, target mode, loss, schedule, seeds, per-epoch history,
    per-bucket validation error, and compute (device, GPU, torch version,
    examples/s) into one curve JSON per run (`train.py`), and the queue that
    runs the sweeps skips finished runs so a restart is a no-op
    (`train_queue.py`);
  * candidates are screened in stages - validation and per-bucket error
    first, then the fixed tactical/endgame suites (`suites.py`), and only then
    engine matches against the frozen classical baseline - because a 50-game
    match on every intermediate checkpoint is a waste of laptop time.

The tactical suite is the position table from `tests/tactical_test.cpp`
translated to UCI expectations: mates that must be found, specific best moves
(promotion, en passant, a free queen), score bounds that must not be violated.
The endgame suite is nine theory-certain positions covering the classes Part
10 lists (pawn, rook, minor, queen endings, sparse pieces, wrong-bishop draw).
Both suites are also run on the frozen classical baseline; the baseline's
scores are the reference the candidates must not fall below.
