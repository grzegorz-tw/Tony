<#
.SYNOPSIS  Configure, build, test and package Tony with Visual Studio 2022 (Desktop development with C++ workload).
.EXAMPLE   .\scripts\build.ps1                 # builds build\Release\tony.exe
.EXAMPLE   .\scripts\build.ps1 -Package        # also creates dist\Tony ready to copy anywhere; then run dist\Tony\scripts\setup.ps1
.EXAMPLE   .\scripts\build.ps1 -Engines        # additionally compile llama.cpp/whisper.cpp/stable-diffusion.cpp (needs the Vulkan SDK)
#>
[CmdletBinding()]
param([ValidateSet('Release', 'Debug')][string]$Config = 'Release', [switch]$Engines, [switch]$Package, [switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

function Find-CMake {
  $c = Get-Command cmake -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -property installationPath
    $p = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path $p) { return $p }
  }
  throw 'CMake not found. Install Visual Studio 2022 with "Desktop development with C++" (includes CMake) or install CMake from cmake.org.'
}
$cmake = Find-CMake
$preset = if ($Engines) { 'vs2022-engines' } else { 'vs2022' }
Write-Host "Using $cmake, preset $preset ($Config)" -ForegroundColor Cyan

& $cmake --preset $preset
if ($LASTEXITCODE) { throw 'configure failed' }
& $cmake --build --preset $(if ($Engines) { 'release-engines' } else { if ($Config -eq 'Debug') { 'debug' } else { 'release' } })
if ($LASTEXITCODE) { throw 'build failed' }
if (-not $SkipTests) {
  & $cmake --build build --config $Config --target ALL_BUILD | Out-Null
  & (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir build -C $Config --output-on-failure
  if ($LASTEXITCODE) { Write-Warning 'some tests failed' }
}
if ($Package) {
  & $cmake --install build --config $Config --prefix (Join-Path $root 'dist\Tony')
  Write-Host "`nPackaged to dist\Tony. Run dist\Tony\scripts\setup.ps1 there to download engines and models." -ForegroundColor Green
} else {
  Write-Host "`nBuilt: build\$Config\tony.exe  (copy scripts\ next to it, or use -Package)" -ForegroundColor Green
}
