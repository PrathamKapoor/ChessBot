# Wait for batch 1 to finish, then start batch 1b. Detached, because a waiter
# parented to an interactive shell dies when that shell's job is reclaimed.
$ErrorActionPreference = "Continue"
$ROOT = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ROOT
while (-not (Test-Path "nnue/curves/NNUE-V2-EPOCHS40.json")) {
  Start-Sleep -Seconds 60
}
Start-Process -FilePath "py" -WindowStyle Hidden `
  -RedirectStandardOutput "nnue/queue_v2b.log" -RedirectStandardError "nnue/queue_v2b.err.log" `
  -ArgumentList "-3 -u nnue/train_queue.py --manifest nnue/phase7_queue_v2b.json"
Add-Content -Path "nnue/queue_v2b.log" -Value "batch 1b launched by watchdog"
