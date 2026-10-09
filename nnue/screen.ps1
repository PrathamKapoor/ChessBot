# Screen one trained checkpoint: export, golden parity, suites.
#
#   pwsh nnue/screen.ps1 -Checkpoint nnue/checkpoints/v2tanh.pt -Name v2tanh
#
# Artefacts:
#   nnue/checkpoints/<name>.bin                PCNN v1 export
#   nnue/reports_phase7_golden_<name>.txt      PyTorch vs C++ parity output
#   nnue/reports_phase7_suite_<name>.json      tactical + endgame suite result
#   nnue/screen_<name>.log / .err.log          step log and engine stderr noise
#
# Each step runs via Start-Process with file redirection on purpose: the engine
# writes NNUEstats to stderr, and piping a native command's stderr through
# PowerShell's pipeline turns that into a terminating error record under
# ErrorActionPreference=Stop (which aborted the first version of this script
# mid-suite). Redirection at the process level keeps the error stream out of
# PowerShell's way.
#
# eval_labels.py is deliberately not here: it needs the split the net was held
# out from, which is not knowable from the checkpoint path alone.
param(
  [Parameter(Mandatory = $true)][string]$Checkpoint,
  [Parameter(Mandatory = $true)][string]$Name
)

$ErrorActionPreference = "Stop"
$ROOT = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ROOT
$bin = "nnue/checkpoints/$Name.bin"
$log = "nnue/screen_$Name.log"
$err = "nnue/screen_$Name.err.log"

function Run-Step($title, $argLine, $outFile, $errFile) {
  Add-Content -Path $log -Value "== $title"
  $proc = Start-Process -FilePath "py" -NoNewWindow -Wait -PassThru `
    -ArgumentList $argLine `
    -RedirectStandardOutput $outFile -RedirectStandardError $errFile
  Add-Content -Path $log -Value ("exit={0}" -f $proc.ExitCode)
}

"screen $Name, checkpoint $Checkpoint" | Out-File $log -Encoding utf8
Run-Step "export" "-3 -u nnue/export.py --checkpoint $Checkpoint --output $bin" `
  "nnue/screen_$Name.export.log" $err
Run-Step "golden parity" "-3 -u nnue/golden_test.py --net $bin" `
  "nnue/reports_phase7_golden_$Name.txt" $err
Run-Step "suites" "-3 -u nnue/suites.py --name $Name --setoption `"name NnuePath value $bin`" --out nnue/reports_phase7_suite_$Name.json" `
  "nnue/screen_$Name.suite.log" $err
"== done $Name" | Add-Content -Path $log
Write-Host "screened $Name" -ForegroundColor Green
