param(
    [string]$OutDir = "",
    [string]$ObjDir = "",
    [string]$JavaHome = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutDir) { $OutDir = Join-Path $Root "build\native" }
if (-not $ObjDir) { $ObjDir = Join-Path $Root "build\obj" }

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

if (-not $JavaHome) { $JavaHome = $env:JAVA_HOME }
if (-not $JavaHome -or -not (Test-Path (Join-Path $JavaHome "include\jni.h"))) {
    $fallback = "C:\Program Files\Java\jdk-17"
    if (Test-Path (Join-Path $fallback "include\jni.h")) {
        $JavaHome = $fallback
    } else {
        throw "jni.h not found. Set JAVA_HOME to a JDK."
    }
}

$CubiomesRoot = Join-Path $Root "cubiomes"
if (-not (Test-Path (Join-Path $CubiomesRoot "generator.c"))) {
    throw "cubiomes submodule not found at $CubiomesRoot. Run 'git submodule update --init --recursive' in LowYDripstoneCaveFinderBase."
}

New-Item -ItemType Directory -Force -Path $OutDir, $ObjDir | Out-Null
$env:PATH = "$MsysBin;$(Join-Path $MsysRoot 'usr\bin');$env:PATH"

# Sources include "cubiomes/...", so the include root is the base repository.
$Includes = @(
    "-I$Root",
    "-I$(Join-Path $Root 'jni')",
    "-I$(Join-Path $JavaHome 'include')",
    "-I$(Join-Path $JavaHome 'include\win32')"
)
$PreCubiomes = Join-Path $Root "cxx_compat\pre_cubiomes.h"

$CSources = @(
    "cubiomes\generator.c",
    "cubiomes\layers.c",
    "cubiomes\biomenoise.c",
    "cubiomes\biomes.c",
    "cubiomes\noise.c",
    "cubiomes\util.c"
)

$CppSources = @(
    "lowydripstonecavefinder.cpp",
    "BiomeSampler.cpp",
    "OctaveFieldCache.cpp",
    "jni\sunnyslopes_lowydscavefinder_LowYDripstoneCaveFinderBridge.cpp"
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

$Dll = Join-Path $OutDir "libDripstoneCaveFinderLibJ.dll"
Write-Host "LINK $Dll"
& $Gxx -shared -static -pthread -o $Dll @Objs -static-libgcc -static-libstdc++ "-Wl,--kill-at"
if ($LASTEXITCODE -ne 0) { throw "link failed" }
Write-Host "Built $Dll"
Get-Item $Dll | Format-List FullName, Length, LastWriteTime
