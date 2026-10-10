# Run the four cross-label evaluations for the matched teacher pair.
#
#   powershell -NoProfile -File nnue/teacher_analysis.ps1
#
# Both arms trained on v4_sub30k.json (depth-7 labels) and v4_sub30k_d9.json
# (the same positions relabelled at depth 9) with the same game-level split,
# so both are held out from exactly the same records. Scoring each net against
# each label set gives the four numbers the teacher experiment needs:
#
#   d7 net vs d7 labels   its own training metric, for reference
#   d7 net vs d9 labels   how close the shallow teacher's function is to the
#                         deep teacher's verdicts
#   d9 net vs d9 labels   its own
#   d9 net vs d7 labels   symmetry
#
# Splits come from v4_sub30k.json (the splitter is deterministic given the
# records and the seed), so every row evaluates the same validation records.
$ErrorActionPreference = "Continue"
$ROOT = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ROOT

$pairs = @(
  @{ net = "nnue/checkpoints/t30k_d7.pt"; data = "nnue/data/v4_sub30k.json";    tag = "d7net_vs_d7" },
  @{ net = "nnue/checkpoints/t30k_d7.pt"; data = "nnue/data/v4_sub30k_d9.json"; tag = "d7net_vs_d9" },
  @{ net = "nnue/checkpoints/t30k_d9.pt"; data = "nnue/data/v4_sub30k_d9.json"; tag = "d9net_vs_d9" },
  @{ net = "nnue/checkpoints/t30k_d9.pt"; data = "nnue/data/v4_sub30k.json";    tag = "d9net_vs_d7" }
)

foreach ($p in $pairs) {
  $out = "nnue/reports_phase7_teacher_$($p.tag).json"
  Write-Host "eval $($p.tag)" -ForegroundColor Cyan
  & py -3 nnue/eval_labels.py --net $p.net --data $p.data `
      --split-data nnue/data/v4_sub30k.json --split-mode game --seed 42 `
      --out $out > "nnue/eval_$($p.tag).log" 2>&1
}

Write-Host "teacher analysis done" -ForegroundColor Green
