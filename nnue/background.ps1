# Launches the two long Phase 7 CPU jobs side by side and reports progress.
#
# The engine's Threads option maxes out at 1, so every labelling worker is a
# single-threaded process and the machine has 8 physical cores (16 logical).
# Worker counts are therefore a partition of a fixed budget, not a scale factor:
# the first attempt ran 12 + 3 = 15 engines, pinned the CPU at 100% and bought
# no extra labelling throughput, because past the physical core count the workers
# simply take fractions of the same cores. 8 + 2 keeps the machine responsive for
# the GPU training runs that share it.
param(
  [switch]$Start,
  [switch]$Status,
  [switch]$Stop
)

$ErrorActionPreference = "Stop"
$jobs = @(
  @{ name = "v4pool"; pid = "nnue\_v4pool.pid" },
  @{ name = "agree"; pid = "nnue\_agree.pid" }
)

if ($Stop) {
  foreach ($j in $jobs) {
    if (Test-Path $j.pid) {
      $id = [int](Get-Content $j.pid)
      Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
      Write-Host "stopped $($j.name) (pid $id)" -ForegroundColor Yellow
    }
    Get-Process pocketchess -ErrorAction SilentlyContinue |
      Stop-Process -Force -ErrorAction SilentlyContinue
    Remove-Item $j.pid -Force -ErrorAction SilentlyContinue
  }
  return
}

if ($Start) {
  $v4 = Start-Process -FilePath "py" -NoNewWindow -PassThru `
    -RedirectStandardOutput "nnue\gen_v4.log" -RedirectStandardError "nnue\gen_v4.err.log" `
    -ArgumentList @(
      "-3","-u","nnue\generate_v3.py",
      "--output","nnue/data/v4_pool.json",
      "--selfplay-games","6800","--random-games","1200",
      "--depth","7","--shallow-depth","3","--selfplay-depth","3",
      "--min-ply","8","--harvest-every","3",
      "--harvest-every-late","3","--late-from-ply","36",
      "--max-plies","140","--opening-min","4","--opening-max","10",
      "--seed","43","--workers","8")
  Set-Content -Path "nnue\_v4pool.pid" -Value $v4.Id
  Write-Host "v4 pool generation started, pid $($v4.Id), 8 workers" -ForegroundColor Green

  $ta = Start-Process -FilePath "py" -NoNewWindow -PassThru `
    -RedirectStandardOutput "nnue\agreement.log" -RedirectStandardError "nnue\agreement.err.log" `
    -ArgumentList @(
      "-3","-u","nnue\teacher_agreement.py",
      "--data","nnue/data/v3_pool.json",
      "--positions","400","--depths","5,7,8,9,10",
      "--depths-deep","11","--deep-subset","200","--workers","2",
      "--out","nnue/reports_phase7_teacher_agreement.json")
  Set-Content -Path "nnue\_agree.pid" -Value $ta.Id
  Write-Host "teacher agreement started, pid $($ta.Id), 2 workers" -ForegroundColor Green
  return
}

Write-Host "=== processes ==="
Get-Process python,py,pocketchess -ErrorAction SilentlyContinue |
  Measure-Object | ForEach-Object { "python/py/pocketchess alive: $($_.Count)" }
Write-Host ("cpu load: {0}%" -f (Get-CimInstance Win32_Processor |
  Measure-Object -Property LoadPercentage -Average).Average)

foreach ($log in @("nnue\gen_v4.log", "nnue\agreement.log")) {
  if (Test-Path $log) {
    Write-Host ""
    Write-Host "=== $log ===" -ForegroundColor Cyan
    Get-Content $log -Tail 6
  }
}
foreach ($log in @("nnue\gen_v4.err.log", "nnue\agreement.err.log")) {
  if ((Test-Path $log) -and (Get-Item $log).Length -gt 0) {
    Write-Host ""
    Write-Host "=== $log ===" -ForegroundColor Red
    Get-Content $log -Tail 10
  }
}