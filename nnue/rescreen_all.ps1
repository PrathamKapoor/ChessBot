# Re-export, re-verify and re-suite every batch-1/1b arm from its FINAL
# checkpoint.
#
# Why this exists: watch_and_screen.ps1 waited for the checkpoint file to
# exist, but the trainer writes it on every validation improvement, so the
# screen fired on the first save while training continued. The exported .bin
# was then a mid-training checkpoint. Golden parity does not catch this - it
# compares the .bin against the .pt at that moment, and both were consistent -
# only v1ctrl's happened to be caught by an overwrite race. Every match that
# used such a .bin is invalid and must be re-run; the offline eval_labels
# numbers are unaffected because they read the .pt, not the .bin.
#
# This script re-exports from the now-final checkpoints, re-runs parity (now a
# real freshness check, since training is over) and re-runs the suites.
$ErrorActionPreference = "Continue"
$ROOT = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ROOT

$arms = @(
  "v2ctrl", "v2mse", "v2tanh", "v2lr2x", "v2ep40",
  "v2clip1000", "v2clip4000", "v2tanh1000", "v2noxtreme", "v2lrhalf",
  "v2tanh500", "v1ctrl"
)

foreach ($name in $arms) {
  $ckpt = "nnue/checkpoints/$name.pt"
  if (-not (Test-Path $ckpt)) { Write-Host "skip $name (no checkpoint)"; continue }
  # Guard: a checkpoint that changed in the last 3 minutes belongs to a run
  # that is still going. None should be, but this is what went wrong before.
  $age = ((Get-Date) - (Get-Item $ckpt).LastWriteTime).TotalSeconds
  if ($age -lt 180) { Write-Host "WAIT $name (checkpoint is $([int]$age)s old)"; Start-Sleep -Seconds 180 }

  Write-Host "== $name export" -ForegroundColor Cyan
  & py -3 nnue/export.py --checkpoint $ckpt --output "nnue/checkpoints/$name.bin" |
    Out-File "nnue/rescreen_$name.log" -Encoding utf8
  Add-Content "nnue/rescreen_$name.log" "== golden parity"
  & py -3 nnue/golden_test.py --net "nnue/checkpoints/$name.bin" 2>&1 |
    Out-File "nnue/reports_phase7_golden_$name.txt" -Encoding utf8
  $parity = Select-String -Path "nnue/reports_phase7_golden_$name.txt" -Pattern "GOLDEN (OK|FAILED)" |
    Select-Object -Last 1
  Add-Content "nnue/rescreen_$name.log" ("parity: " + $parity.Line.Trim())
  Write-Host ("  parity: " + $parity.Line.Trim())

  Add-Content "nnue/rescreen_$name.log" "== suites"
  & py -3 nnue/suites.py --name $name --setoption "name NnuePath value nnue/checkpoints/$name.bin" `
      --out "nnue/reports_phase7_suite_$name.json" 2>&1 |
    Out-File "nnue/rescreen_$name.suite.log" -Encoding utf8
}
Write-Host "rescreen done" -ForegroundColor Green
