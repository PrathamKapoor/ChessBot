# Research baseline

Snapshot: 2026-10-02. Every claim below was checked against a primary source
during this session. Where a widely-repeated claim turned out to be wrong, the
correction is recorded rather than quietly dropped.

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
