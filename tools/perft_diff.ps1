# Diffs the engine's per-move perft against python-chess.
#
#   pwsh tools/perft_diff.ps1 -Case kiwipete -Depth 2
#
# Reports root moves the engine fails to generate, root moves it generates that
# the reference rejects, and roots whose subtree count disagrees.

param(
  [Parameter(Mandatory = $true)][string]$Case,
  [int]$Depth = 2
)

$ErrorActionPreference = "Stop"

$refFile = Join-Path $env:TEMP ("pcref_" + $Case + ".txt")
& py -3 tools/reference_perft.py --case $Case --depth $Depth |
  Out-File -FilePath $refFile -Encoding utf8

# Engine CSV: case,depth,move,count -- move may carry a trailing promo column.
$mine = @{}
foreach ($line in (& ./build/perft.exe --divides --depth $Depth 2>&1)) {
  $p = $line.Trim() -split ','
  if ($p.Count -ge 4 -and $p[0] -eq $Case) { $mine["$($p[1])|$($p[2].Trim())"] = [int]$p[3] }
}

$ref = @{}
$d = 0
foreach ($line in (Get-Content $refFile)) {
  if ($line -match '^=== depth (\d+) ===') { $d = [int]$Matches[1]; continue }
  if ($line -match '^\s+(\S{4}) : (\d+)') { $ref["$d|$($Matches[1])"] = [int]$Matches[2] }
}

Write-Host "case=$Case depth=$Depth  engine=$($mine.Count) ref=$($ref.Count)" -ForegroundColor Cyan

$missing = @($ref.Keys | Where-Object { -not $mine.ContainsKey($_) } | Sort-Object)
$extra = @($mine.Keys | Where-Object { -not $ref.ContainsKey($_) } | Sort-Object)
$wrong = @($ref.Keys | Where-Object { $mine.ContainsKey($_) -and $mine[$_] -ne $ref[$_] } | Sort-Object)

if ($missing.Count -gt 0) {
  Write-Host "MISSING - engine does not generate:" -ForegroundColor Yellow
  foreach ($k in $missing) { Write-Host "  $k" }
}
if ($extra.Count -gt 0) {
  Write-Host "EXTRA - engine generates, reference does not:" -ForegroundColor Red
  foreach ($k in $extra) { Write-Host "  $k  engineSubtree=$($mine[$k])" }
}
if ($wrong.Count -gt 0) {
  Write-Host "MISMATCHED subtree counts:" -ForegroundColor Magenta
  foreach ($k in $wrong) {
    Write-Host ("  {0}  engine={1} ref={2} delta={3}" -f $k, $mine[$k], $ref[$k], ($mine[$k] - $ref[$k]))
  }
}

if ($missing.Count -eq 0 -and $extra.Count -eq 0 -and $wrong.Count -eq 0) {
  Write-Host "DIVIDE MATCHES REFERENCE" -ForegroundColor Green
}
