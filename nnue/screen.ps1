# Screen one trained checkpoint: export, golden parity, suites.
#
# Run it detached or in the foreground; every step writes its own artefact:
#   pwsh nnue/screen.ps1 -Checkpoint nnue/checkpoints/v2tanh.pt -Name v2tanh
#
#   nnue/checkpoints/<name>.bin                PCNN v1 export
#   nnue/reports_phase7_golden_<name>.txt      parity + pinned-golden output
#   nnue/reports_phase7_suite_<name>.json      tactical + endgame suite result
#   nnue/screen_<name>.log                     step-by-step log
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

"== export $Name" | Out-File $log -Encoding utf8
& py -3 nnue/export.py --checkpoint $Checkpoint --output $bin 2>&1 |
  Tee-Object -FilePath $log -Append

"== golden parity $Name" | Tee-Object -FilePath $log -Append
& py -3 nnue/golden_test.py --net $bin 2>&1 |
  Tee-Object -FilePath "nnue/reports_phase7_golden_$Name.txt"

"== suites $Name" | Tee-Object -FilePath $log -Append
& py -3 nnue/suites.py --name $Name `
  --setoption "name NnuePath value $bin" `
  --out "nnue/reports_phase7_suite_$Name.json" 2>&1 |
  Tee-Object -FilePath $log -Append
"== done $Name" | Tee-Object -FilePath $log -Append
