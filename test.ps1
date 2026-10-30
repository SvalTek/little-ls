param(
    [string]$Compiler = "",
    [string]$LittleRoot = ""
)

$ErrorActionPreference = "Stop"

$repo = $PSScriptRoot
if (-not $LittleRoot) { $LittleRoot = Join-Path (Split-Path -Parent $repo) "little" }
$LittleRoot = (Resolve-Path $LittleRoot).Path

$toolchainBin = if ($env:GCC_PATH) { Join-Path $env:GCC_PATH "bin" } else { "" }
if ($toolchainBin -and (Test-Path $toolchainBin)) {
    $env:PATH = "$toolchainBin$([IO.Path]::PathSeparator)$env:PATH"
}
$Compiler = if ($Compiler) { $Compiler } elseif ($env:GCC_PATH) { Join-Path $env:GCC_PATH "bin/gcc.exe" } else { "gcc" }

$outDir = Join-Path $repo "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir "analysis-tests.exe"

& $Compiler -std=c11 -O1 -g `
    -I (Join-Path $LittleRoot "src") `
    (Join-Path $repo "tests/analysis.c") `
    (Join-Path $LittleRoot "src/little.c") `
    (Join-Path $LittleRoot "src/little_analysis.c") `
    (Join-Path $LittleRoot "src/little_buffer.c") `
    (Join-Path $LittleRoot "src/little_common.c") `
    (Join-Path $LittleRoot "src/little_async.c") `
    -o $exe

if ($LASTEXITCODE -ne 0) {
    throw "Test build failed with exit code $LASTEXITCODE"
}

& $exe
if ($LASTEXITCODE -ne 0) {
    throw "Analysis tests failed with exit code $LASTEXITCODE"
}
