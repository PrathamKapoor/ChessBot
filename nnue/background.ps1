# Launches the long Phase 7 CPU jobs detached and reports progress.
#
# The engine's Threads option maxes out at 1, so every labelling worker is a
# single-threaded process and the machine has 8 physical cores (16 logical).
# Worker counts are therefore a partition of a fixed budget, not a scale factor:
# an earlier attempt ran 12 + 3 = 15 engines, pinned the CPU at 100% and bought
# no extra labelling throughput, because past the physical core count the
# workers simply take fractions of the same cores. 14 stays inside the budget
# and keeps the machine responsive for the GPU training runs that share it.
#
# Jobs write their own log under nnue/ and leave a pid file so -Status and
# -Stop can find them. Detached (not a child of this shell): a machine restart
# kills them, which is survivable only because generate_v3.py and relabel.py
# shard every chunk to disk and resume with --resume.
param(
  [string]$Start = "",
  [switch]$Status,
  [switch]$Stop
)

$ErrorActionPreference = "Stop"
$ROOT = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

$jobs = @{
  # Clean pool, fixed generator: odd harvest strides only (16e7cd1), depth 7
  # teacher with a depth-3 shallow label for the tactical volatility proxy.
  pool = @{
    pid = "nnue\_pool.pid"
    log = "nnue\gen_v4.log"
    err = "nnue\gen_v4.err.log"
    args = @(
      "-3","-u","nnue\generate_v3.py",
      "--output","nnue/data/v4_pool.json",
      "--selfplay-games","15500","--random-games","2500",
      "--depth","7","--shallow-depth","3","--selfplay-depth","3",
      "--min-ply","8","--harvest-every","3",
      "--harvest-every-late","3","--late-from-ply","40",
      "--max-plies","140","--opening-min","4","--opening-max","10",
      "--seed","44","--workers","14",
      # Always resume: a restart after a machine reboot continues from the
      # shards instead of failing the fresh-start guard, and on a first run
      # --resume is a no-op because there is no shard directory yet.
      "--resume")
  }
  # Teacher throughput at the deeper rungs, on a subset with game identity.
  rate = @{
    pid = "nnue\_rate.pid"
    log = "nnue\rate.log"
    err = "nnue\rate.err.log"
    args = @(
      "-3","-u","nnue\teacher_rate.py",
      "--data","nnue/data/v3_sub20k.json",
      "--depths","9,10,11","--positions","32","--workers","14",
      "--out","nnue/reports_phase7_teacher_rate_deep.json")
  }
  # The teacher experiment's d9 arm: relabel the exact 30k subset the d7 arm
  # trains on, keeping the original depth-7 labels alongside (--keep-original).
  relabel9 = @{
    pid = "nnue\_relabel9.pid"
    log = "nnue\relabel_d9.log"
    err = "nnue\relabel_d9.err.log"
    args = @(
      "-3","-u","nnue\relabel.py",
      "--data","nnue/data/v4_sub30k.json",
      "--depth","9","--keep-original",
      "--out","nnue/data/v4_sub30k_d9.json",
      "--workers","14","--resume")
  }
  # Agreement ladder at the rungs that cannot carry a training set: d10 and
  # d11 on a 512-position slice, one engine session per position.
  ladder = @{
    pid = "nnue\_ladder.pid"
    log = "nnue\ladder.log"
    err = "nnue\ladder.err.log"
    args = @(
      "-3","-u","nnue\relabel.py",
      "--data","nnue/data/v4_sub30k.json",
      "--depth","10","--also-depth","11",
      "--limit","512","--keep-original",
      "--out","nnue/data/v4_ladder512.json",
      "--workers","14","--resume")
  }
}

if ($Stop) {
  foreach ($name in $jobs.Keys) {
    $j = $jobs[$name]
    if (Test-Path (Join-Path $ROOT $j.pid)) {
      $id = [int](Get-Content (Join-Path $ROOT $j.pid))
      Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
      Write-Host "stopped $name (pid $id)" -ForegroundColor Yellow
    }
    Remove-Item (Join-Path $ROOT $j.pid) -Force -ErrorAction SilentlyContinue
  }
  Get-Process pocketchess -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
  return
}

if ($Status) {
  Write-Host "=== processes ==="
  Get-Process python,py,pocketchess -ErrorAction SilentlyContinue |
    Measure-Object | ForEach-Object { "python/py/pocketchess alive: $($_.Count)" }
  Write-Host ("cpu load: {0}%" -f (Get-CimInstance Win32_Processor |
    Measure-Object -Property LoadPercentage -Average).Average)
  foreach ($name in $jobs.Keys) {
    $log = Join-Path $ROOT $jobs[$name].log
    if (Test-Path $log) {
      Write-Host ""
      Write-Host "=== $name : $($jobs[$name].log) ===" -ForegroundColor Cyan
      Get-Content $log -Tail 5
    }
  }
  return
}

if ($Start -eq "") {
  Write-Host "usage: background.ps1 -Start <jobname> | -Status | -Stop"
  Write-Host "jobs: $($jobs.Keys -join ', ')"
  return
}

if (-not $jobs.ContainsKey($Start)) {
  throw "unknown job '$Start'; jobs: $($jobs.Keys -join ', ')"
}

$j = $jobs[$Start]
$proc = Start-Process -FilePath "py" -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $ROOT $j.log) `
  -RedirectStandardError (Join-Path $ROOT $j.err) `
  -WorkingDirectory $ROOT `
  -ArgumentList $j.args
Set-Content -Path (Join-Path $ROOT $j.pid) -Value $proc.Id
Write-Host "started $Start, pid $($proc.Id)" -ForegroundColor Green
