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
$engineEval = $engine + @("engine/evaluate.cpp")
$engineSearch = $engineEval + @("engine/tt.cpp", "engine/search.cpp")
$engineUci = $engineSearch + @("engine/uci.cpp")

# Diagnostic accounting is a compile-time switch (see engine/types.hpp). The
# production binary is built with it off so that no counter code is emitted at
# all; the _instr binary exists only to be measured against it.
$noInstrument = @("-DPOCKET_INSTRUMENT=0")
$instrument = @("-DPOCKET_INSTRUMENT=1")

# Test binaries are always built instrumented. They are not used for performance
# measurement, and this keeps the counter code paths compiled and executed rather
# than silently rotting behind a switch nobody ever flips.
$testFlags = $instrument

$targets = @(
  @{ name = "perft";     sources = @("tools/perft.cpp") + $engine;              flags = $noInstrument },
  @{ name = "bench";     sources = @("tools/bench.cpp") + $engine;              flags = $noInstrument },
  @{ name = "bitboard_test"; sources = @("tests/bitboard_test.cpp") + @("engine/bitboard.cpp"); flags = $testFlags },
  @{ name = "geometry_test"; sources = @("tests/geometry_test.cpp") + @("engine/bitboard.cpp"); flags = $testFlags },
  @{ name = "eval_test"; sources = @("tests/eval_test.cpp") + $engineEval;      flags = $testFlags },
  @{ name = "tt_test"; sources = @("tests/tt_test.cpp") + @("engine/tt.cpp");  flags = $testFlags },
  @{ name = "search_test"; sources = @("tests/search_test.cpp") + $engineUci;   flags = $testFlags },
  @{ name = "pocketchess"; sources = @("tools/engine_main.cpp") + $engineUci;    flags = $noInstrument },
  @{ name = "search_bench"; sources = @("tools/search_bench.cpp") + $engineSearch; flags = $noInstrument },
  @{ name = "search_bench_instr"; sources = @("tools/search_bench.cpp") + $engineSearch; flags = $instrument }
)

New-Item -ItemType Directory -Force -Path "build" | Out-Null

$failed = @()
foreach ($t in $targets) {
  $out = "build/$($t.name).exe"
  Write-Host "==> $out" -ForegroundColor Cyan
  & $compiler @common @opt @($t.flags) @($t.sources) -o $out 2>&1 | ForEach-Object { $_ }
  if ($LASTEXITCODE -ne 0) { $failed += $t.name }
}

if ($failed.Count -gt 0) {
  Write-Host "BUILD FAILED: $($failed -join ', ')" -ForegroundColor Red
  exit 1
}
Write-Host "build ok" -ForegroundColor Green
