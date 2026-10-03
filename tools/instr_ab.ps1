# Interleaved A/B measurement of the instrumentation overhead.
#
# Phase 3A produced nodes-per-second figures from a build whose counters sat in
# the hot path, and never measured what those counters cost. This script answers
# that question directly.
#
# The method matters more than the numbers. Comparing one run of one binary
# against one run of the other would measure the machine's thermal state rather
# than the code: this laptop was observed drifting 47% across three identical
# repetitions. So the two binaries are alternated within a single sweep, every
# repetition sees the same conditions, and node counts are checked for equality
# as well as time being measured. If node counts agree and times differ, the
# difference is attributable to the code rather than to the workload.
#
# Usage:
#   pwsh tools/instr_ab.ps1 -BinaryA build/bench_prod.exe -BinaryB build/bench_instr.exe -Depth 5 -Rounds 6
param(
  [string]$BinaryA = "build/bench_prod.exe",
  [string]$BinaryB = "build/bench_instr.exe",
  [int]$Depth = 5,
  [int]$Rounds = 6
)

$ErrorActionPreference = "Stop"

function Get-Row([string]$exe, [int]$depth) {
  # A single repetition, parsed out of the machine-readable rep line.
  $out = & $exe --bench --depth $depth --reps 1 2>&1 | Out-String
  $line = ($out -split "`n" | Where-Object { $_ -match '^\s*rep0\s' } | Select-Object -First 1)
  if (-not $line) { throw "could not parse output of $exe`n$out" }
  $nodes  = [int64]([regex]::Match($line, 'nodes=(\d+)').Groups[1].Value)
  $qnodes = [int64]([regex]::Match($line, 'qnodes=(\d+)').Groups[1].Value)
  $ms     = [int64]([regex]::Match($line, 'time=(\d+)').Groups[1].Value)
  [pscustomobject]@{ Nodes = $nodes; QNodes = $qnodes; Ms = $ms }
}

Write-Host "interleaved A/B: instrumentation overhead" -ForegroundColor Cyan
Write-Host "  A (production, no counters) = $BinaryA"
Write-Host "  B (instrumented)           = $BinaryB"
Write-Host "  depth=$Depth rounds=$Rounds"
Write-Host ""

$rows = @()
for ($i = 0; $i -lt $Rounds; $i++) {
  # Alternate the order each round as well, so that a monotonic thermal drift
  # during the sweep cannot systematically favour whichever binary ran first.
  if ($i % 2 -eq 0) {
    $a = Get-Row $BinaryA $Depth
    $b = Get-Row $BinaryB $Depth
  } else {
    $b = Get-Row $BinaryB $Depth
    $a = Get-Row $BinaryA $Depth
  }
  $rows += [pscustomobject]@{ Round = $i; A = $a; B = $b }
  Write-Host ("round {0}: A nodes={1} {2}ms   B nodes={3} {4}ms" -f $i, $a.Nodes, $a.Ms, $b.Nodes, $b.Ms)
}

$aMs     = ($rows | ForEach-Object { $_.A.Ms } | Measure-Object -Minimum -Maximum -Average)
$bMs     = ($rows | ForEach-Object { $_.B.Ms } | Measure-Object -Minimum -Maximum -Average)
$aMed    = ($rows | ForEach-Object { $_.A.Ms } | Sort-Object)[[int]($Rounds / 2)]
$bMed    = ($rows | ForEach-Object { $_.B.Ms } | Sort-Object)[[int]($Rounds / 2)]
$aNodes  = ($rows | Select-Object -First 1).A.Nodes
$bNodes  = ($rows | Select-Object -First 1).B.Nodes
$qnA     = ($rows | Select-Object -First 1).A.QNodes
$qnB     = ($rows | Select-Object -First 1).B.QNodes

$nodesAgree = ($rows | Where-Object { $_.A.Nodes -ne $_.B.Nodes }).Count -eq 0

Write-Host ""
Write-Host "--- summary, depth $Depth ---"
Write-Host ("A production   nodes={0} qnodes={1}  min={2}ms mean={3:N0}ms median={4}ms max={5}ms" -f `
  $aNodes, $qnA, $aMs.Minimum, $aMs.Average, $aMed, $aMs.Maximum)
Write-Host ("B instrumented nodes={0} qnodes={1}  min={2}ms mean={3:N0}ms median={4}ms max={5}ms" -f `
  $bNodes, $qnB, $bMs.Minimum, $bMs.Average, $bMed, $bMs.Maximum)

if ($nodesAgree) {
  Write-Host "node counts identical in every round: the two builds searched the same tree"

  # Throttling makes the mean and median of wall clock unreliable: this machine
  # was observed producing two distinct clusters of timings within a single
  # sweep, so a median can land in the wrong cluster entirely. The minimum is the
  # standard robust estimator for this situation, because the fastest run is the
  # one that was least disturbed. Both are reported, and the overhead is quoted
  # against the minimum with the spread given alongside so the reader can see
  # whether the overhead even rises above the noise.
  $overheadMin  = if ($aMs.Minimum -gt 0) { 100.0 * ($bMs.Minimum - $aMs.Minimum) / $aMs.Minimum } else { 0 }
  $overheadMean = if ($aMs.Average -gt 0) { 100.0 * ($bMs.Average - $aMs.Average) / $aMs.Average } else { 0 }
  $spreadA = if ($aMs.Minimum -gt 0) { 100.0 * ($aMs.Maximum - $aMs.Minimum) / $aMs.Minimum } else { 0 }

  Write-Host ("overhead vs minimum : {0:+0.0;-0.0}%   ({1}ms -> {2}ms)" -f $overheadMin, $aMs.Minimum, $bMs.Minimum)
  Write-Host ("overhead vs mean    : {0:+0.0;-0.0}%   ({1:N0}ms -> {2:N0}ms)" -f $overheadMean, $aMs.Average, $bMs.Average)
  Write-Host ("within-binary spread: {0:N1}%  <- noise floor; an overhead below this is not resolved" -f $spreadA)

  $npsA = if ($aMs.Minimum -gt 0) { $aNodes / $aMs.Minimum } else { 0 }
  $npsB = if ($bMs.Minimum -gt 0) { $bNodes / $bMs.Minimum } else { 0 }
  Write-Host ("trustworthy NPS (production,   min-time): {0:N0} main nodes/s" -f $npsA)
  Write-Host ("trustworthy NPS (instrumented, min-time): {0:N0} main nodes/s" -f $npsB)
  $qnA = 0
  Write-Host ("NOTE qnodes are reported as 0 by the production build by design: the counter is compiled out,")
  Write-Host ("     which is the whole point of the exercise. Node count remains the trusted metric.")
} else {
  Write-Host "NODE COUNTS DIFFER between builds - the comparison is invalid until that is explained"
}