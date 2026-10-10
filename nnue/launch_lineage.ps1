# Wait for batch 1b's last arm, then run the lineage control (v1's 38k records
# with the Phase 7 recipe) so the GPU never sits idle between batches.
$ErrorActionPreference = "Continue"
$ROOT = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ROOT
while (-not (Test-Path "nnue/curves/NNUE-V2-LRHALF.json")) {
  Start-Sleep -Seconds 60
}
Start-Process -FilePath "py" -WindowStyle Hidden `
  -RedirectStandardOutput "nnue/queue_lineage.log" -RedirectStandardError "nnue/queue_lineage.err.log" `
  -ArgumentList "-3 -u nnue/train_queue.py --manifest nnue/phase7_queue_lineage.json"
Add-Content -Path "nnue/queue_lineage.log" -Value "lineage control launched by watchdog"
