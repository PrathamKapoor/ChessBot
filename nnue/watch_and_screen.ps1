# Screen checkpoints as they appear: export, golden parity, suites, eval_labels.
#
#   powershell -NoProfile -File nnue/watch_and_screen.ps1 `
#       -Names v2lr2x,v2ep40 -Data nnue/data/v2.json -SplitMode record
#
# Waits for each checkpoint file (training runs finish on their own schedule),
# then runs the full offline screen. eval_labels needs the split the net was
# held out from, which is why --Data/--SplitMode/--Seed are parameters:
# v2-substrate arms use record split seed 42, the clean-pool arms game split.
param(
  [Parameter(Mandatory = $true)][string]$Names,
  [string]$Data = "nnue/data/v2.json",
  [string]$SplitMode = "record",
  [int]$Seed = 42
)

$ErrorActionPreference = "Continue"
$ROOT = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ROOT

foreach ($name in $Names.Split(",")) {
  $ckpt = "nnue/checkpoints/$name.pt"
  while (-not (Test-Path $ckpt)) { Start-Sleep -Seconds 60 }
  Start-Sleep -Seconds 15   # let the trainer finish writing the checkpoint

  Write-Host "screening $name" -ForegroundColor Cyan
  & powershell -NoProfile -File nnue/screen.ps1 -Checkpoint $ckpt -Name $name

  & py -3 nnue/eval_labels.py --net $ckpt --data $Data --split-data $Data `
      --split-mode $SplitMode --seed $Seed `
      --out "nnue/reports_phase7_labels_$name.json" > "nnue/eval_$name.log" 2>&1
  Write-Host "done $name" -ForegroundColor Green
}
