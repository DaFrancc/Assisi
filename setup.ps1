# tools/setup.ps1
# Usage: powershell -ExecutionPolicy Bypass -File tools/setup.ps1

$ErrorActionPreference = "Stop"

# -----------------------
# Pretty output helpers
# -----------------------
function Write-Info([string]$Message)    { Write-Host "[i] $Message" -ForegroundColor Cyan }
function Write-Ok([string]$Message)      { Write-Host "[+] $Message" -ForegroundColor Green }
function Write-Warn([string]$Message)    { Write-Host "[!] $Message" -ForegroundColor Yellow }
function Write-Err([string]$Message)     { Write-Host "[x] $Message" -ForegroundColor Red }
function Write-Heading([string]$Message) { Write-Host "`n=== $Message ===" -ForegroundColor Magenta }

function Read-NonEmpty([string]$Prompt) {
  while ($true) {
    $v = Read-Host $Prompt
    if (-not [string]::IsNullOrWhiteSpace($v)) { return $v.Trim() }
    Write-Warn "Please enter a value."
  }
}

function Validate-GameName([string]$Name) {
  if ($Name -notmatch '^[A-Za-z][A-Za-z0-9_-]*$') {
    throw "Game name '$Name' is invalid. Use letters/numbers/underscore/dash, start with a letter."
  }
}

$repoRoot = (Get-Location).Path
Set-Location $repoRoot

if (-not (Test-Path (Join-Path $repoRoot "CMakePresets.json"))) {
  throw "Run this from the repository root (the folder containing CMakePresets.json). Current: $repoRoot"
}

Write-Heading "Assisi Game Setup"
Write-Info "Repo: $repoRoot"

# 1) Prompt user
$gameName = Read-NonEmpty "Game name (e.g. CustomGame)"
Validate-GameName $gameName

$gameDir = Join-Path $repoRoot "apps\$gameName"
$srcDir  = Join-Path $gameDir "src"

if (Test-Path $gameDir) {
  throw "apps/$gameName already exists."
}

Write-Heading "Scaffolding"
Write-Info "Creating: apps/$gameName"

# 2) Scaffold new app (minimal)
New-Item -ItemType Directory -Force -Path $srcDir | Out-Null

@"
add_executable(Assisi-$gameName)
Assisi_apply_defaults(Assisi-$gameName)

target_sources(Assisi-$gameName
  PRIVATE
    `"src/main.cpp`"
)

find_package(OpenGL REQUIRED)

target_link_libraries(Assisi-$gameName
  PRIVATE
    Assisi::Render
    Assisi::ECS
    Assisi::Game
    Assisi::Window
    Assisi::Deps
    OpenGL::GL
)
"@ | Set-Content -Encoding UTF8 (Join-Path $gameDir "CMakeLists.txt")

@"
#include <iostream>

int main() {
  std::cout << "${gameName}: Hello from Assisi!" << std::endl;
  return 0;
}
"@ | Set-Content -Encoding UTF8 (Join-Path $srcDir "main.cpp")

Write-Ok "Created apps/$gameName with a starter CMakeLists.txt and main.cpp"

# 3) Create/update CMakeUserPresets.json to select app without editing committed presets
Write-Heading "User Presets"
$userPresetsPath = Join-Path $repoRoot "CMakeUserPresets.json"

$userPresets = @{
  version = 6
  configurePresets = @(
    @{
      name = "msvc-debug-user"
      inherits = "msvc-debug"
      cacheVariables = @{ ASSISI_APP = $gameName }
    },
    @{
      name = "msvc-release-user"
      inherits = "msvc-release"
      cacheVariables = @{ ASSISI_APP = $gameName }
    },
    @{
      name = "msvc-sanitize-user"
      inherits = "msvc-sanitize"
      cacheVariables = @{ ASSISI_APP = $gameName }
    }
  )
  buildPresets = @(
    @{ name = "msvc-debug-user"; configurePreset = "msvc-debug-user" },
    @{ name = "msvc-release-user"; configurePreset = "msvc-release-user" },
    @{ name = "msvc-sanitize-user"; configurePreset = "msvc-sanitize-user" }
  )
}

($userPresets | ConvertTo-Json -Depth 10) | Set-Content -Encoding UTF8 $userPresetsPath
Write-Ok "Wrote CMakeUserPresets.json (selects ASSISI_APP=$gameName)"

# 4) Kick off Conan installs as background jobs (logs per preset)
Write-Heading "Dependencies (Conan)"
$profilesDir = Join-Path $repoRoot "profiles"
$buildRoot   = Join-Path $repoRoot "out\build"

$jobs = @()

function Start-ConanInstallJob([string]$preset) {
  $outDir = Join-Path $buildRoot $preset
  $profilePath = Join-Path $profilesDir $preset
  $logPath = Join-Path $outDir "conan-install.log"

  if (-not (Test-Path $profilePath)) {
    Write-Err "Missing profile: $profilePath"
    throw "Profile file not found for preset '$preset'. Expected: $profilePath"
  }

  New-Item -ItemType Directory -Force -Path $outDir | Out-Null

  $cmd = "conan install . -of=`"$outDir`" -pr:h=`"$profilePath`" -pr:b=`"$profilePath`" -g CMakeDeps -g CMakeToolchain --build=missing"
  Write-Info "Starting Conan: $preset"
  Start-Job -Name "conan-$preset" -ScriptBlock {
    param($repoRoot, $cmd, $logPath)
    Set-Location $repoRoot
    & cmd.exe /c $cmd *>&1 | Out-File -FilePath $logPath -Encoding utf8
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  } -ArgumentList $repoRoot, $cmd, $logPath
}

$jobs += Start-ConanInstallJob "msvc-debug"
$jobs += Start-ConanInstallJob "msvc-release"
$jobs += Start-ConanInstallJob "msvc-sanitize"

Write-Ok "Conan installs running in the background."
Write-Host ""
Write-Info "You can configure/build now with:"
Write-Host "  cmake --preset msvc-debug-user" -ForegroundColor White
Write-Host "  cmake --build --preset msvc-debug-user" -ForegroundColor White
Write-Host ""

# 5) Wait and report results
Write-Heading "Waiting for Conan"
Wait-Job $jobs | Out-Null

$failed = @()
foreach ($j in $jobs) {
  if ($j.State -ne "Completed") { $failed += $j.Name }
  Receive-Job $j | Out-Null
  Remove-Job $j | Out-Null
}

if ($failed.Count -gt 0) {
  Write-Err ("Some Conan installs failed: " + ($failed -join ", "))
  Write-Warn "Check logs under: out/build/<preset>/conan-install.log"
  throw "Conan failed."
}

Write-Ok "All Conan installs completed successfully."
Write-Heading "Done"
Write-Ok "Game: apps/$gameName"
Write-Ok "Preset: msvc-debug-user"
