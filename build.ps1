# Configure + build helper. Wraps vcvars64 so clang-cl finds the MSVC headers,
# libs and the Windows SDK.
#
#   .\build.ps1                              configure (if needed) and build everything
#   .\build.ps1 -Target mc2_stuff
#   .\build.ps1 -Fresh                       wipe the build dir first
#   .\build.ps1 -Compiler cl                 cross-check with MSVC instead of clang-cl
#   .\build.ps1 -Config RelWithDebInfo       see the CRT note below
#   .\build.ps1 -WebGPU                      link Dawn (implies RelWithDebInfo)
#   .\build.ps1 -Emscripten                  build headless WebAssembly + JavaScript
#   .\build.ps1 -WebGPU -Symbols             same, but with real debug info
#
# SYMBOLS -- read this before you try to debug a hang.
#
# Despite the name, RelWithDebInfo in this project carries NO debug information:
# no /Zi at compile, no /DEBUG at link. bin\MechCmd2.pdb is a stale leftover that
# matches no binary, so every frame of every stack symbolises as <no symbol>. That
# cost one session hours before it was noticed.
#
# -Symbols configures a SEPARATE tree with /Z7 and /DEBUG added and nothing else
# changed -- same optimisation settings, same defines, so behaviour is identical
# and a stack taken there is valid for the normal build. It is a separate dir on
# purpose: the shipped configs and their published references stay untouched.
#
#   .\build.ps1 -WebGPU -Symbols
#   tools\debug\mc2stack.exe <pid> 5 800     # attach to a live/hung process
#
# CRT MATCHING -- this is why -Config exists.
#
# Dawn is built Release, which uses the /MD runtime. A Debug build of this
# project uses /MDd. Linking objects against two different CRTs gives you two
# heaps in one process: memory allocated by one is freed by the other, and the
# result is corruption that looks nothing like its cause. So the moment Dawn is
# linked, this project has to leave Debug.
#
# RelWithDebInfo is the right pairing: /MD like Dawn, but with symbols. -WebGPU
# selects it automatically so the mismatch cannot be made by accident.

param(
  [string]$Target   = "",
  [switch]$Fresh,
  [ValidateSet("clang-cl","cl")]
  [string]$Compiler = "clang-cl",
  [ValidateSet("Debug","RelWithDebInfo","Release")]
  [string]$Config   = "Debug",
  [switch]$WebGPU,
  [switch]$Symbols,
  [switch]$Emscripten,
  [string]$LogFile  = ""
)

$ErrorActionPreference = "Continue"
$root   = $PSScriptRoot
# Any Visual Studio 2022 edition with the C++ workload works (Community, Build
# Tools, ...): ask vswhere where it is, and fall back to the Community default.
$vsRoot  = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
  $found = & $vswhere -latest -products * -version "[17.0,18.0)" `
             -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($found) { $vsRoot = @($found)[0] }
}
$vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"

# TOOLSET PINNING -- do not remove without reading this.
#
# clang-cl finds MSVC itself and always picks the NEWEST installed toolset for its
# headers. vcvars64.bat, left alone, exports LIB for whichever toolset it considers
# default -- here 14.38 while clang-cl compiles against 14.44. Headers and import
# libraries then disagree, and the build only fails when some translation unit
# happens to use a library feature newer than the link libraries: std::find_last_of
# in vfs_resolve.cpp compiles to __std_find_last_trivial_1, which exists only in
# 14.44's msvcprt.lib, so the link died with "undefined symbol" while every compile
# succeeded. The mismatch was latent for the whole project and surfaced on the first
# relink after that file changed.
#
# Pin vcvars to the same toolset clang-cl chose, so headers and libs always agree.
$vcToolsRoot = Join-Path $vsRoot "VC\Tools\MSVC"
$newestToolset = (Get-ChildItem $vcToolsRoot -Directory -ErrorAction SilentlyContinue |
                  Sort-Object { [version]$_.Name } | Select-Object -Last 1).Name
$vcvarsArgs = if ($newestToolset) { " -vcvars_ver=$newestToolset" } else { "" }

if ($WebGPU -and $Config -eq "Debug") {
  Write-Host "[build] -WebGPU: switching Config to RelWithDebInfo to match Dawn's /MD runtime"
  $Config = "RelWithDebInfo"
}

# Separate build dirs per (compiler, config) so switching never half-rebuilds
# against stale objects compiled with a different CRT.
$suffix = if ($Emscripten) { "emscripten-$Config" } else { "$Compiler-$Config" }
if ($WebGPU)  { $suffix += "-webgpu" }
if ($Symbols) { $suffix += "-sym" }
$build = Join-Path $root "build-$suffix"

if ($Fresh -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }

if ($Emscripten) {
  $emsdk = Join-Path $root "..\third_party\dawn\third_party\emsdk"
  $emsdkEnv = Join-Path $emsdk "emsdk_env.ps1"
  $emcmake = Join-Path $emsdk "upstream\emscripten\emcmake.bat"
  if (!(Test-Path $emsdkEnv) -or !(Test-Path $emcmake)) {
    throw "Project-local emsdk is not installed at $emsdk"
  }

  # PORT: activation is scoped to this PowerShell process. Never modify the
  # user's persistent PATH or system environment for the browser build.
  . $emsdkEnv | Out-Host
  $configureArgs = @(
    "cmake", "-S", $root, "-B", $build, "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$Config",
    # PORT: M5-5 browser builds exercise the real emdawnwebgpu renderer.
    "-DMC2_ENABLE_WEBGPU=ON",
    "-DMC2_BUILD_TESTS=OFF"
  )
  & $emcmake @configureArgs
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  $buildArgs = @("--build", $build)
  if ($Target) { $buildArgs += @("--target", $Target) }
  $buildArgs += @("--", "-k", "0")
  if ($LogFile) { & cmake @buildArgs 2>&1 | Tee-Object -FilePath $LogFile }
  else { & cmake @buildArgs }
  exit $LASTEXITCODE
}

$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" +
            [System.Environment]::GetEnvironmentVariable("Path","User")

$cfg = "cmake -S `"$root`" -B `"$build`" -G Ninja " +
       "-DCMAKE_BUILD_TYPE=$Config " +
       "-DCMAKE_C_COMPILER=$Compiler -DCMAKE_CXX_COMPILER=$Compiler"
if ($WebGPU) { $cfg += " -DMC2_ENABLE_WEBGPU=ON" } else { $cfg += " -DMC2_ENABLE_WEBGPU=OFF" }
if ($Symbols) {
  # /Z7 puts debug info in the objects and /DEBUG makes the linker emit a matching
  # PDB. Nothing else is added -- in particular NOT /O2, so codegen is unchanged
  # and a stack from this tree describes the normal build.
  # The empty base-flag overrides matter: a fresh configure would otherwise pick up
  # CMake's MSVC defaults (/DWIN32 /D_WINDOWS /EHsc), which the long-lived normal
  # build dirs do not carry. Clearing them keeps this tree flag-for-flag identical
  # to the shipped config apart from /Z7 and /DEBUG.
  $cfg += " -DCMAKE_C_FLAGS= -DCMAKE_CXX_FLAGS=" +
          " -DCMAKE_C_FLAGS_$($Config.ToUpper())=/Z7" +
          " -DCMAKE_CXX_FLAGS_$($Config.ToUpper())=/Z7" +
          " -DCMAKE_EXE_LINKER_FLAGS=/DEBUG"
  Write-Host "[build] -Symbols: adding /Z7 + /DEBUG in $build (codegen unchanged)"
}

$bld = "cmake --build `"$build`""
if ($Target) { $bld += " --target $Target" }
# Keep going after errors so one build surfaces the whole error population
# rather than stopping at the first file.
$bld += " -- -k 0"

if ($newestToolset) { Write-Host "[build] MSVC toolset pinned to $newestToolset (headers and libs must match)" }
$cmd = "`"$vcvars`"$vcvarsArgs >nul 2>&1 && $cfg && $bld"

if ($LogFile) { cmd /c $cmd 2>&1 | Tee-Object -FilePath $LogFile }
else          { cmd /c $cmd 2>&1 }
