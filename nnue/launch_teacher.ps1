# Wait for the depth-9 relabel of the teacher subset, then start the teacher
# arms. The relabel takes hours (depth 9 on this machine is ~1 position/s per
# worker on the clean pool's positions) and its output appearing is the only
# signal that the matched pair (d7 labels / d9 labels) is ready.
$ErrorActionPreference = "Continue"
$ROOT = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ROOT
while (-not (Test-Path "nnue/data/v4_sub30k_d9.json")) {
  Start-Sleep -Seconds 120
}
Start-Sleep -Seconds 30   # let the writer finish its rename
Start-Process -FilePath "py" -WindowStyle Hidden `
  -RedirectStandardOutput "nnue/queue_teacher.log" -RedirectStandardError "nnue/queue_teacher.err.log" `
  -ArgumentList "-3 -u nnue/train_queue.py --manifest nnue/phase7_queue_teacher.json"
Add-Content -Path "nnue/queue_teacher.log" -Value "teacher queue launched by watchdog"
