param(
    [string]$OutDir = "",
    [string]$ObjDir = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutDir) { $OutDir = Join-Path $Root "build\cli" }
if (-not $ObjDir) { $ObjDir = Join-Path $Root "build\cli-obj" }

$MsysRoot = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { "C:\msys64" }
$MsysCandidates = @(
    (Join-Path $MsysRoot "ucrt64\bin"),
    (Join-Path $MsysRoot "mingw64\bin")
)
$MsysBin = $MsysCandidates | Where-Object {
    (Test-Path (Join-Path $_ "gcc.exe")) -and (Test-Path (Join-Path $_ "g++.exe"))
} | Select-Object -First 1
if (-not $MsysBin) {
    throw "MSYS2 gcc/g++ not found under $MsysRoot\ucrt64\bin or $MsysRoot\mingw64\bin"
}
$Gcc = Join-Path $MsysBin "gcc.exe"
$Gxx = Join-Path $MsysBin "g++.exe"

$CubiomesRoot = Join-Path $Root "cubiomes"
if (-not (Test-Path (Join-Path $CubiomesRoot "generator.c"))) {
    throw "cubiomes submodule not found at $CubiomesRoot. Run 'git submodule update --init --recursive' in LowYDripstoneCaveFinderBase."
}

New-Item -ItemType Directory -Force -Path $OutDir, $ObjDir | Out-Null
$env:PATH = "$MsysBin;$(Join-Path $MsysRoot 'usr\bin');$env:PATH"

# Sources include "cubiomes/...", so the include root is the base repository.
$Includes = @(
    "-I$Root"
)
$PreCubiomes = Join-Path $Root "cxx_compat\pre_cubiomes.h"

# cubiomes core used by the cave/river search (finders/loot are not needed).
$CSources = @(
    "cubiomes\generator.c",
    "cubiomes\layers.c",
    "cubiomes\biomenoise.c",
    "cubiomes\biomes.c",
    "cubiomes\noise.c",
    "cubiomes\util.c"
)

# lowydripstonecavefinder.cpp #includes dripstonecave_finder.cpp and
# LargePitSearch.cpp; do not compile those separately or symbols duplicate.
$CppSources = @(
    "lowydripstonecavefinder.cpp",
    "BiomeSampler.cpp",
    "OctaveFieldCache.cpp",
    "lowydripstonecavefinder_cli.cpp"
)

$Objs = @()
foreach ($src in $CSources) {
    $name = [IO.Path]::GetFileNameWithoutExtension($src)
    $obj = Join-Path $ObjDir "$name.o"
    Write-Host "CC  $src"
    & $Gcc -O3 -DNDEBUG -D_WIN32 -std=c17 -ffp-contract=off @Includes -c (Join-Path $Root $src) -o $obj
    if ($LASTEXITCODE -ne 0) { throw "gcc failed on $src" }
    $Objs += $obj
}

foreach ($src in $CppSources) {
    $name = [IO.Path]::GetFileNameWithoutExtension($src)
    $obj = Join-Path $ObjDir "$name.o"
    Write-Host "CXX $src"
    & $Gxx -O3 -DNDEBUG -D_WIN32 -std=c++20 -ffp-contract=off -include $PreCubiomes @Includes -c (Join-Path $Root $src) -o $obj
    if ($LASTEXITCODE -ne 0) { throw "g++ failed on $src" }
    $Objs += $obj
}

$Exe = Join-Path $OutDir "lowydripstonecavefinder-cli.exe"
Write-Host "LINK $Exe"
# -lpsapi: --bench reports the process peak working set (GetProcessMemoryInfo).
& $Gxx -static -pthread -o $Exe @Objs -static-libgcc -static-libstdc++ -lpsapi
if ($LASTEXITCODE -ne 0) { throw "link failed" }
Write-Host "Built $Exe"
Get-Item $Exe | Format-List FullName, Length, LastWriteTime
