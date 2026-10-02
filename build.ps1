# Builds every target with the host MSYS2 g++.
#
# The CPU here is Zen 3: AVX2 + BMI2 (PEXT) are available, AVX-512 is not. That
# combination drives two decisions elsewhere in the codebase: PEXT magic
# bitboards for move generation, and maddubs-based int8 NNUE inference instead
# of VNNI.
param(
  [string]$Config = "release"
)

$ErrorActionPreference = "Stop"

$compiler = "g++"
if (-not (Get-Command $compiler -ErrorAction SilentlyContinue)) {
  throw "g++ not found on PATH. Install MSYS2 UCRT64 and add its bin directory to PATH."
}

$common = @("-std=c++20", "-I.", "-Wall", "-Wextra", "-Wno-unused-parameter")
if ($Config -eq "release") {
  $opt = @("-O3", "-DNDEBUG", "-mavx2", "-mbmi", "-mbmi2", "-mpopcnt", "-funroll-loops")
} else {
  $opt = @("-O0", "-g", "-mavx2", "-mbmi", "-mbmi2", "-mpopcnt")
}

$engine = @("engine/bitboard.cpp", "engine/position.cpp", "engine/movegen.cpp")

$targets = @(
  @{ name = "perft";     sources = @("tools/perft.cpp") + $engine },
  @{ name = "bitboard_test"; sources = @("tests/bitboard_test.cpp") + @("engine/bitboard.cpp") }
)

New-Item -ItemType Directory -Force -Path "build" | Out-Null

$failed = @()
foreach ($t in $targets) {
  $out = "build/$($t.name).exe"
  Write-Host "==> $out" -ForegroundColor Cyan
  & $compiler @common @opt @($t.sources) -o $out 2>&1 | ForEach-Object { $_ }
  if ($LASTEXITCODE -ne 0) { $failed += $t.name }
}

if ($failed.Count -gt 0) {
  Write-Host "BUILD FAILED: $($failed -join ', ')" -ForegroundColor Red
  exit 1
}
Write-Host "build ok" -ForegroundColor Green
