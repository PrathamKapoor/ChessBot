#!/usr/bin/env python3
"""Sequential Probability Ratio Test for engine A/B experiments.

WHAT THIS IS
------------
A SPRT is a sequential test between two hypotheses about a parameter. Unlike a
fixed-sample confidence interval it answers as soon as the evidence is strong
enough, and it controls the error rates of both wrong answers directly rather
than approximately. That is what engine testing needs: after forty games a large
effect should already be concluded, and a null effect should not be declared
until there is genuinely no evidence either way.

HYPOTHESES
----------
    H0: the true score is at most  elo0   (no advantage demonstrated)
    H1: the true score is at least elo1   (candidate is better)

with elo0 < elo1, normally elo0 = 0 and elo1 > 0.

THE LIKELIHOOD, EXACTLY
-----------------------
p_H is the score the hypothesis predicts. Under the standard engine-testing model
the decisive games are a Bernoulli stream with win probability p_H, and the
log-likelihood ratio over the observed decisive games is

    LLR = wins * ln(p1 / p0) + losses * ln((1 - p1) / (1 - p0))

Note what is NOT in that expression: draws. They cancel because the draw rate is
treated as common to both hypotheses.

THE DRAW ASSUMPTION, STATED PLAINLY BECAUSE IT IS NOT FREE
----------------------------------------------------------
Cancelling the draws assumes the draw rate does not depend on which hypothesis is
true. That is an approximation, and it is the weakest assumption in this file.

It is standard practice in engine testing and it is defensible when the two engines
play at a similar draw rate, which is the common case for a small search or
evaluation change. It is NOT defensible without comment when a candidate changes
the draw rate materially, for example by introducing repetition avoidance. In that
case the effective number of decisive games is smaller than the game count, a
match takes longer than this model predicts, and the reported error rates are
optimistic.

The draw count is recorded in the report so this can be checked rather than
assumed.

THE TWO BOUNDARIES
-------------------
    upper  ln((1 - beta) / alpha)    LLR above this accepts H1
    lower  ln(beta / (1 - alpha))    LLR below this accepts H0

Getting this wrong is not subtle in the code but it is easy to get wrong in
practice: with alpha = beta = 0.05 the lower boundary is NEGATIVE, so using it as
a single threshold accepts H1 after one game. That was the first bug in this
implementation and the validation below is what caught it.
"""
import argparse
import json
import math
import sys

# Chess scores map to Elo through the logistic model, which is what the per-game
# win/draw/loss model implies.
#   elo_diff = -400 * log10(1/p - 1)   for a score p in (0, 1)


def elo_to_score(elo):
    return 1.0 / (1.0 + 10.0 ** (-elo / 400.0))


def score_to_elo(score):
    if score <= 0.0:
        return float("-inf")
    if score >= 1.0:
        return float("inf")
    return -400.0 * math.log10(1.0 / score - 1.0)


class Sprt:
    """Two-hypothesis SPRT over a sequence of game scores in {0, 0.5, 1}."""

    def __init__(self, elo0=0.0, elo1=5.0, alpha=0.05, beta=0.05):
        if not (elo1 > elo0):
            raise ValueError("elo1 must be greater than elo0")
        if not (0.0 < alpha < 1.0) or not (0.0 < beta < 1.0):
            raise ValueError("alpha and beta must be probabilities")
        self.elo0 = float(elo0)
        self.elo1 = float(elo1)
        self.alpha = float(alpha)
        self.beta = float(beta)
        self.p0 = elo_to_score(self.elo0)
        self.p1 = elo_to_score(self.elo1)
        self.upper = math.log((1.0 - self.beta) / self.alpha)
        self.lower = math.log(self.beta / (1.0 - self.alpha))
        self.reset()

    def reset(self):
        self.games = 0
        self.wins = 0
        self.draws = 0
        self.losses = 0
        self.llr = 0.0
        self.decided = None

    def add(self, score):
        """Add one game result: 1.0 win, 0.5 draw, 0.0 loss."""
        if score not in (0.0, 0.5, 1.0):
            raise ValueError(f"score must be 0, 0.5 or 1, got {score!r}")
        self.games += 1
        if score == 1.0:
            self.wins += 1
        elif score == 0.5:
            self.draws += 1
        else:
            self.losses += 1

        # Written with explicit guards so that a run of all draws, all wins or
        # all losses does not evaluate log(0). Each term is multiplied by a count
        # that is exactly zero in the case that would make the logarithm
        # undefined, so skipping the term is exact rather than an approximation.
        total = 0.0
        if self.wins:
            total += self.wins * math.log(self.p1 / self.p0)
        if self.losses:
            total += self.losses * math.log((1.0 - self.p1) / (1.0 - self.p0))
        self.llr = total

        if self.llr > self.upper:
            self.decided = "H1"
        elif self.llr < self.lower:
            self.decided = "H0"
        return self.decided

    def score(self):
        if self.games == 0:
            return 0.0
        return (self.wins + 0.5 * self.draws) / self.games

    def decisive_games(self):
        return self.wins + self.losses

    def elo_estimate(self):
        return score_to_elo(self.score())

    def status(self):
        if self.decided == "H1":
            return "H1 accepted: candidate is better"
        if self.decided == "H0":
            return "H0 accepted: no advantage demonstrated"
        return "inconclusive: continue"

    def report(self):
        return {
            "elo0": self.elo0,
            "elo1": self.elo1,
            "alpha": self.alpha,
            "beta": self.beta,
            "games": self.games,
            "wins": self.wins,
            "draws": self.draws,
            "losses": self.losses,
            "decisive_games": self.decisive_games(),
            "draw_rate": (self.draws / self.games) if self.games else 0.0,
            "score": self.score(),
            "elo_estimate": self.elo_estimate(),
            "llr": self.llr,
            "upper_boundary": self.upper,
            "lower_boundary": self.lower,
            "decision": self.decided or "inconclusive",
            "status": self.status(),
        }


def simulate(n_games, truth_elo, elo1, elo0=0.0, alpha=0.05, beta=0.05,
             draw_rate=0.6, seed=12345):
    """Deterministic pseudo-simulation used only to validate the test itself.

    This is NOT a strength measurement and cannot be one. It feeds the test a
    sequence generated from a KNOWN truth so that the test's own behaviour can be
    checked: a clearly better candidate should trend to H1, a clearly worse one to
    H0, and equal ones should stay inconclusive rather than producing a false
    acceptance.

    Data are generated from truth_elo, which is deliberately independent of the
    hypotheses. Generating from elo1 would be circular and would prove nothing.
    """
    import random
    rng = random.Random(seed)
    sprt = Sprt(elo0, elo1, alpha, beta)

    # The test's parameter is the win probability among DECISIVE games, expressed
    # as an Elo difference. The simulation must use exactly the same
    # interpretation or the validation proves nothing, which is what went wrong
    # on the first version: it generated decisive wins at a rate derived from the
    # draw rate while the test compared against the undiluted parameter.
    win_prob = elo_to_score(truth_elo)

    for _ in range(n_games):
        r = rng.random()
        if r < draw_rate:
            sprt.add(0.5)
        elif r < draw_rate + (1.0 - draw_rate) * win_prob:
            sprt.add(1.0)
        else:
            sprt.add(0.0)
        if sprt.decided:
            break
    return sprt


def main():
    ap = argparse.ArgumentParser(
        description="Sequential Probability Ratio Test for engine experiments.")
    ap.add_argument("--elo0", type=float, default=0.0,
                    help="score below which H0 holds (default 0)")
    ap.add_argument("--elo1", type=float, default=5.0,
                    help="score at or above which H1 holds (default 5)")
    ap.add_argument("--alpha", type=float, default=0.05,
                    help="probability of wrongly accepting H1 (default 0.05)")
    ap.add_argument("--beta", type=float, default=0.05,
                    help="probability of wrongly accepting H0 (default 0.05)")
    ap.add_argument("--games", type=int, default=0,
                    help="simulate this many games to demonstrate the test")
    ap.add_argument("--truth", type=float, default=0.0,
                    help="Elo the simulated results come from. The hypotheses "
                         "come from --elo0 and --elo1.")
    ap.add_argument("--draw-rate", type=float, default=0.6)
    ap.add_argument("--seed", type=int, default=12345)
    args = ap.parse_args()

    if args.games:
        sprt = simulate(args.games, args.truth, args.elo1, args.elo0,
                        args.alpha, args.beta, args.draw_rate, args.seed)
    else:
        sprt = Sprt(args.elo0, args.elo1, args.alpha, args.beta)
    print(json.dumps(sprt.report(), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())