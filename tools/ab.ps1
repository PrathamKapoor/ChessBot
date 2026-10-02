# Interleaved A/B benchmark for two prebuilt engine binaries.
#
# Run-to-run variance on this host is up to 20% because of thermal behaviour on
# a mobile part, which is larger than most single optimizations. Measuring A
# fully, then B, therefore proves nothing. This harness alternates the two
# binaries so any drift in clock or temperature affects both equally, and it
# reports the best (not just the mean) time because throttling only ever makes a
# run slower.
#
#   pwsh tools/ab.ps1 -Base build/base/perft.exe -Cand build/cand/perft.exe -Reps 7
param(
  [Parameter(Mandatory = $true)][string]$Base,
  [Parameter(Mandatory = $true)][string]$Cand,
  [int]$Reps = 7,
  [string]$Label = "positional d5",
  [string]$Fen = "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
  [int]$Depth = 5
)

$ErrorActionPreference = "Stop"

function Measure-Run([string]$exe) {
  # The perft tool prints one line of the form: nodes=N  time=T s  nps=X
  $out = & $exe --fen $Fen --depth $Depth 2>&1 | Select-String -Pattern 'nodes=(\d+)\s+time=([\d.]+)s\s+nps=(\d+)'
  if (-not $out) { throw "no output from $exe" }
  $m = $out.Matches[0]
  return [pscustomobject]@{
    Nodes = [double]$m.Groups[1].Value
    Time  = [double]$m.Groups[2].Value
    Nps   = [double]$m.Groups[3].Value
  }
}

# Warm both binaries so neither pays for cold start / cold caches.
$null = Measure-Run $Base
$null = Measure-Run $Cand

$b = @(); $c = @()
for ($i = 0; $i -lt $Reps; $i++) {
  $b += Measure-Run $Base
  $c += Measure-Run $Cand
}

# Node counts must match exactly, otherwise this is a correctness problem and
# the timings are meaningless.
$bn = ($b | Select-Object -First 1).Nodes
$cn = ($c | Select-Object -First 1).Nodes
if ($bn -ne $cn) {
  Write-Host "NODE COUNT MISMATCH: base=$bn cand=$cn -- correctness failure, timings invalid" -ForegroundColor Red
  exit 1
}

$bBest = ($b | Sort-Object Time | Select-Object -First 1)
$cBest = ($c | Sort-Object Time | Select-Object -First 1)
$bMed  = ($b | Sort-Object Time)[[int]($b.Count / 2)]
$cMed  = ($c | Sort-Object Time)[[int]($c.Count / 2)]

$gainBest = 100.0 * ($bBest.Time - $cBest.Time) / $bBest.Time
$gainMed  = 100.0 * ($bMed.Time  - $cMed.Time)  / $bMed.Time

Write-Host ""
Write-Host "=== A/B: $Label  (interleaved, $Reps reps, nodes=$([int64]$bn)) ===" -ForegroundColor Cyan
Write-Host ("{0,-10} {1,12} {2,14} {3,14}" -f "", "nodes", "best s", "median s")
Write-Host ("{0,-10} {1,12} {2,14:N4} {3,14:N4}" -f "base", [int64]$bn, $bBest.Time, $bMed.Time)
Write-Host ("{0,-10} {1,12} {2,14:N4} {3,14:N4}" -f "cand", [int64]$cn, $cBest.Time, $cMed.Time)
Write-Host ""
Write-Host ("  nps base  best {0:N0}   median {1:N0}" -f $bBest.Nps, $bMed.Nps)
Write-Host ("  nps cand  best {0:N0}   median {1:N0}" -f $cBest.Nps, $cMed.Nps)
Write-Host ("  DELTA     best {0:N2}%   median {1:N2}%" -f $gainBest, $gainMed) -ForegroundColor Green
