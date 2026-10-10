# Run NNUE-vs-NNUE A/B matches for a list of arms, each against a fixed
# baseline net, as their exports appear.
#
#   powershell -NoProfile -File nnue/ab_chain.ps1 `
#       -Names v2noxtreme,v2clip1000 -BaselineNet nnue/checkpoints/v2ctrl.bin
#
# Waits for nnue/checkpoints/<name>.bin (written by the screening step), then
# launches the 50-game match. Sequential on purpose: matches share the CPU with
# the pool labelling, and running several at once only makes them all slower.
param(
  [Parameter(Mandatory = $true)][string]$Names,
  [Parameter(Mandatory = $true)][string]$BaselineNet,
  [string]$BaselineName = "v2ctrl",
  [int]$Games = 50,
  [int]$Depth = 5
)

$ErrorActionPreference = "Continue"
$ROOT = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ROOT

foreach ($name in $Names.Split(",")) {
  $bin = "nnue/checkpoints/$name.bin"
  # Wait for the RESCREEN, not for the file: the .bin exists from the moment
  # of the first mid-training export, and a match was run against one of those
  # before this was noticed (see rescreen_all.ps1). The rescreen log's parity
  # line is the evidence that the .bin came from the final checkpoint.
  $rescreen = "nnue/rescreen_$name.log"
  while (-not ((Test-Path $rescreen) -and (Select-String -Path $rescreen -Pattern "parity: GOLDEN" -Quiet))) {
    Start-Sleep -Seconds 60
  }
  # Wait for any earlier match to finish: exactly one match at a time.
  while (Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
         Where-Object { $_.CommandLine -match 'match/runner.py' -or $_.CommandLine -match 'match\\runner.py' }) {
    Start-Sleep -Seconds 60
  }
  Write-Host "starting A/B match $name vs $BaselineName" -ForegroundColor Cyan
  & powershell -NoProfile -File nnue/run_match.ps1 -Id "P7-AB-$name" `
      -BaselineNet $BaselineNet -CandidateNet $bin `
      -BaselineName $BaselineName -CandidateName $name `
      -Games $Games -Depth $Depth
  Start-Sleep -Seconds 30
}
Write-Host "A/B chain done" -ForegroundColor Green
