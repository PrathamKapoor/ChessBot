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
