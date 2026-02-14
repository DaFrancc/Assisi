# tools/setup.ps1
# Usage: powershell -ExecutionPolicy Bypass -File tools/setup.ps1

$ErrorActionPreference = "Stop"
$global:PSNativeCommandUseErrorActionPreference = $false

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

# 4) Conan installs (sequential, no jobs/threads)
Write-Heading "Dependencies (Conan via Make)"

# Ensure make exists
if (-not (Get-Command make -ErrorAction SilentlyContinue)) {
  throw "make not found in PATH. Install MSYS2 make, Ninja, or another make implementation."
}

# Run the Makefile target that performs the conan installs sequentially
Write-Info "Running: make"
& make
if ($LASTEXITCODE -ne 0) {
  throw "make failed (exit $LASTEXITCODE)"
}

Write-Ok "All Conan installs completed successfully."


Write-Host ""
Write-Info "You can configure/build now with:"
Write-Host "  cmake --preset msvc-debug-user" -ForegroundColor White
Write-Host "  cmake --build --preset msvc-debug-user" -ForegroundColor White

Write-Heading "Done"
Write-Ok "Game: apps/$gameName"
Write-Ok "Preset: msvc-debug-user"
