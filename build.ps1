param(
    [string]$Compiler = "",
    [string]$Output = "build/little-ls.exe",
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

$outPath = Join-Path $repo $Output
$outDir = Split-Path -Parent $outPath
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Only the analysis-capable core is linked; the standard library is not needed
# because worker VMs get their initializer from the host instead.
& $Compiler -std=c11 -O2 -Wall -Wextra `
    -I (Join-Path $LittleRoot "src") `
    -I (Join-Path $LittleRoot "vendor/yyjson") `
    -DYYJSON_READER_DEPTH_LIMIT=64 `
    -DYYJSON_WRITER_DEPTH_LIMIT=64 `
    (Join-Path $repo "src/main.c") `
    (Join-Path $repo "src/lsp.c") `
    (Join-Path $repo "src/documents.c") `
    (Join-Path $repo "src/docs.c") `
    (Join-Path $repo "src/index.c") `
    (Join-Path $repo "src/types.c") `
    (Join-Path $LittleRoot "src/little.c") `
    (Join-Path $LittleRoot "src/little_analysis.c") `
    (Join-Path $LittleRoot "src/little_buffer.c") `
    (Join-Path $LittleRoot "src/little_common.c") `
    (Join-Path $LittleRoot "src/little_async.c") `
    (Join-Path $LittleRoot "vendor/yyjson/yyjson.c") `
    -o $outPath

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host "Built $Output"
