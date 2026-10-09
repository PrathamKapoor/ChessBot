# Waits for the clean pool to finish, verifies it, then launches the whole
# post-pool chain. Detached: the pool labelling finishes on its own schedule
# (hours), and every downstream step should start the moment the file exists
# rather than whenever someone happens to look.
#
# Chain:
#   1. verify v4_pool.json (metadata, checksum, colour balance) via the dataset
#      gate - abort everything if it fails, because a bad pool invalidates the
#      relabel and every training run behind it
#   2. diagnose.py -> reports_phase7_pool_diagnostics_v4.json
#   3. dataset_registry.py -> reports_phase7_dataset_registry.json (all datasets)
#   4. make_subset.py -> v4_sub30k.json (30k records, seed 51) for the teacher arms
#   5. background.ps1 -Start relabel9 (14 workers, ~4 h) -> v4_sub30k_d9.json
#   6. train_queue.py --manifest phase7_queue_pool.json (GPU, batch 2)
#
# Log: nnue/postpool.log. Status: nnue/postpool_status.txt (last completed step).
param(
  [int]$PollSeconds = 300,
  [int]$MaxHours = 14
)

$ErrorActionPreference = "Continue"
$ROOT = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ROOT
$log = "nnue/postpool.log"
$status = "nnue/postpool_status.txt"

function Say($msg) {
  $line = "{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $msg
  Add-Content -Path $log -Value $line
}

Say "postpool watcher started; waiting for nnue/data/v4_pool.json"
$deadline = (Get-Date).AddHours($MaxHours)
$pool = "nnue/data/v4_pool.json"

while (-not (Test-Path $pool)) {
  if ((Get-Date) -gt $deadline) {
    Say "TIMEOUT: pool file never appeared after $MaxHours h; giving up"
    Set-Content $status "timeout-waiting-for-pool"
    exit 1
  }
  Start-Sleep -Seconds $PollSeconds
}
# The generator writes via write-then-rename, so existence should imply
# completeness; still give the writer a moment and verify the checksum.
Start-Sleep -Seconds 20
Say "pool file present; verifying"

function Run($title, $argLine) {
  Say "STEP $title : py $argLine"
  $proc = Start-Process -FilePath "py" -NoNewWindow -Wait -PassThru `
    -ArgumentList $argLine `
    -RedirectStandardOutput "nnue/postpool_$title.log" `
    -RedirectStandardError "nnue/postpool_$title.err.log"
  Say "STEP $title exit=$($proc.ExitCode)"
  Set-Content $status $title
  return $proc.ExitCode
}

$code = Run "verify" "-3 -u nnue/test_datasets.py"
if ($code -ne 0) {
  Say "ABORT: dataset gate failed on the new pool; nothing launched"
  Set-Content $status "aborted-dataset-gate-failed"
  exit 1
}

# Label determinism: re-search a sample with a fresh teacher and require exact
# reproduction of both stored labels. This is the check that caught the
# score_cp/shallow_cp swap (check_labels.py, see fix_label_swap.py); a pool
# that fails it must never feed a training run.
$code = Run "labelcheck" "-3 -u nnue/check_labels.py nnue/data/v4_pool.json 8"
if ($code -ne 0) {
  Say "ABORT: label determinism failed on the new pool; nothing launched"
  Set-Content $status "aborted-label-check-failed"
  exit 1
}

Run "diagnose" "-3 -u nnue/diagnose.py nnue/data/v4_pool.json --out nnue/reports_phase7_pool_diagnostics_v4.json" | Out-Null
Run "registry" "-3 -u nnue/dataset_registry.py" | Out-Null
Run "subset30k" "-3 -u nnue/make_subset.py --data nnue/data/v4_pool.json --records 30000 --seed 51 --out nnue/data/v4_sub30k.json" | Out-Null

Say "starting relabel9 (14 workers, d9 on the 30k subset)"
Start-Process -FilePath "powershell" -WindowStyle Hidden `
  -ArgumentList "-NoProfile -File nnue/background.ps1 -Start relabel9"
Say "starting batch-2 queue"
Start-Process -FilePath "py" -WindowStyle Hidden `
  -RedirectStandardOutput "nnue/queue_pool.log" -RedirectStandardError "nnue/queue_pool.err.log" `
  -ArgumentList "-3 -u nnue/train_queue.py --manifest nnue/phase7_queue_pool.json"
Set-Content $status "relabel9-and-batch2-launched"
Say "done: relabel9 and batch-2 launched"
