# Launch a match detached, with correct quoting for setoption-style arguments.
#
# The runner's --candidate-setoption takes a value with spaces, and PowerShell's
# Start-Process -ArgumentList splits an array on spaces (so "name NnuePath
# value X" arrived as four arguments and argparse rejected it). Passing one
# pre-quoted string avoids that.
#
# NNUE vs classical (candidate side loads a net):
#   pwsh nnue/run_match.ps1 -Id P7-SPRT-D500K -CandidateNet nnue/checkpoints/d500k.bin
# NNUE vs NNUE (baseline side loads the control net):
#   pwsh nnue/run_match.ps1 -Id P7-AB-TANH -BaselineNet nnue/checkpoints/v2ctrl.bin `
#       -CandidateNet nnue/checkpoints/v2tanh.bin -Games 50
param(
  [Parameter(Mandatory = $true)][string]$Id,
  [string]$CandidateNet = "",
  [string]$BaselineNet = "",
  [int]$Games = 50,
  [int]$Depth = 5,
  [string]$BaselineName = "classical",
  [string]$CandidateName = "",
  [string]$Suite = "match/suite_core-01.txt"
)

$ErrorActionPreference = "Stop"
$ROOT = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

if ($CandidateName -eq "") {
  $CandidateName = if ($CandidateNet) { [IO.Path]::GetFileNameWithoutExtension($CandidateNet) } else { "candidate" }
}

$a = @("-3", "-u", "match/runner.py",
  "--baseline", "build/pocketchess.exe",
  "--candidate", "build/pocketchess.exe",
  "--baseline-name", $BaselineName,
  "--candidate-name", $CandidateName,
  "--id", $Id,
  "--games", "$Games",
  "--control", "depth", "--depth", "$Depth",
  "--hash-mb", "16", "--max-plies", "150",
  "--suite", $Suite,
  "--baseline-build", "phase7 tree, build under test",
  "--candidate-build", "phase7 tree, build under test")
if ($BaselineNet) { $a += @("--baseline-setoption", "name NnuePath value $BaselineNet") }
if ($CandidateNet) { $a += @("--candidate-setoption", "name NnuePath value $CandidateNet") }

# Quote every element that contains a space, then join into one string for
# Start-Process (which is what makes CreateProcess split it back correctly).
$quoted = $a | ForEach-Object { if ($_ -match ' ') { '"' + $_ + '"' } else { $_ } }
$argLine = $quoted -join ' '

$log = Join-Path $ROOT "nnue\match_$Id.log"
$err = Join-Path $ROOT "nnue\match_$Id.err.log"
$proc = Start-Process -FilePath "py" -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput $log -RedirectStandardError $err `
  -WorkingDirectory $ROOT -ArgumentList $argLine
Write-Host "match $Id started, pid $($proc.Id), log $log" -ForegroundColor Green
