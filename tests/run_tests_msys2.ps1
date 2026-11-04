param(
    [string]$OutDir = "",
    [string]$ObjDir = ""
)

# Builds and runs the B4 native regression tests with the same MSYS2 toolchain
# the DLL/CLI use.  (tests/CMakeLists.txt is the CTest equivalent for CI; this
# script keeps the local workflow dependency-free.)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Base = Split-Path -Parent $Root
if (-not $OutDir) { $OutDir = Join-Path $Base "build\tests-native\bin" }
if (-not $ObjDir) { $ObjDir = Join-Path $Base "build\tests-native\obj" }

$MsysRoot = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { "C:\msys64" }
$MsysBin = @(
    (Join-Path $MsysRoot "ucrt64\bin"),
    (Join-Path $MsysRoot "mingw64\bin")
) | Where-Object {
    (Test-Path (Join-Path $_ "gcc.exe")) -and (Test-Path (Join-Path $_ "g++.exe"))
} | Select-Object -First 1
if (-not $MsysBin) {
    throw "MSYS2 gcc/g++ not found under $MsysRoot\ucrt64\bin or $MsysRoot\mingw64\bin"
}
$Gcc = Join-Path $MsysBin "gcc.exe"
$Gxx = Join-Path $MsysBin "g++.exe"

New-Item -ItemType Directory -Force -Path $OutDir, $ObjDir | Out-Null
$env:PATH = "$MsysBin;$(Join-Path $MsysRoot 'usr\bin');$env:PATH"

$Includes = @("-I$Base")
$PreCubiomes = Join-Path $Base "cxx_compat\pre_cubiomes.h"

$CSources = @(
    "cubiomes\generator.c",
    "cubiomes\layers.c",
    "cubiomes\biomenoise.c",
    "cubiomes\biomes.c",
    "cubiomes\noise.c",
    "cubiomes\util.c"
)
$EngineSources = @(
    "lowydripstonecavefinder.cpp",
    "BiomeSampler.cpp",
    "OctaveFieldCache.cpp",
    "GpuCoarse.cpp"
)

$CObjs = @()
foreach ($src in $CSources) {
    $obj = Join-Path $ObjDir ("test-" + [IO.Path]::GetFileNameWithoutExtension($src) + ".o")
    Write-Host "CC  $src"
    & $Gcc -O3 -DNDEBUG -D_WIN32 -std=c17 -ffp-contract=off @Includes -c (Join-Path $Base $src) -o $obj
    if ($LASTEXITCODE -ne 0) { throw "gcc failed on $src" }
    $CObjs += $obj
}

$EngineObjs = @()
foreach ($src in $EngineSources) {
    $obj = Join-Path $ObjDir ("test-" + [IO.Path]::GetFileNameWithoutExtension($src) + ".o")
    Write-Host "CXX $src"
    & $Gxx -O3 -DNDEBUG -D_WIN32 -std=c++20 -ffp-contract=off -include $PreCubiomes @Includes -c (Join-Path $Base $src) -o $obj
    if ($LASTEXITCODE -ne 0) { throw "g++ failed on $src" }
    $EngineObjs += $obj
}

function Build-Test([string]$name, [string[]]$objects) {
    $obj = Join-Path $ObjDir ("test-" + $name + ".o")
    Write-Host "CXX tests\$name.cpp"
    & $Gxx -O3 -DNDEBUG -D_WIN32 -std=c++20 -ffp-contract=off -include $PreCubiomes @Includes -c (Join-Path $Root "$name.cpp") -o $obj
    if ($LASTEXITCODE -ne 0) { throw "g++ failed on tests/$name.cpp" }
    $exe = Join-Path $OutDir "$name.exe"
    Write-Host "LINK $exe"
    & $Gxx -static -pthread -o $exe @objects $obj -static-libgcc -static-libstdc++ -lpsapi
    if ($LASTEXITCODE -ne 0) { throw "link failed for $name" }
    return $exe
}

$volumeExe = Build-Test "volume_unit" ($CObjs + $EngineObjs)
$columnExe = Build-Test "column_reference" $CObjs

Write-Host "`n=== volume_unit ==="
& $volumeExe
if ($LASTEXITCODE -ne 0) { throw "volume_unit failed" }

Write-Host "`n=== column_reference ==="
& $columnExe (Join-Path $Root "drowned-original-reference.tsv")
if ($LASTEXITCODE -ne 0) { throw "column_reference failed" }

Write-Host "`nall native regression tests passed"
