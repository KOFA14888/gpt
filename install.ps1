# ScreenMate installer — Windows
# Usage (PowerShell):
#   iwr https://raw.githubusercontent.com/KOFA14888/gpt/arena/d7d4a28f-gpt/install.ps1 -UseBasicParsing | iex
$ErrorActionPreference = "Stop"

$Repo    = "https://github.com/KOFA14888/gpt"
$Branch  = "arena/d7d4a28f-gpt"   # default branch has no sources yet
$Asset   = "$Repo/releases/latest/download/screenmate-windows.zip"
$BinDir  = Join-Path $env:LOCALAPPDATA "ScreenMate\bin"
$ExePath = Join-Path $BinDir "screenmate.exe"

Write-Host "ScreenMate installer (Windows)" -ForegroundColor Cyan

# 1) prebuilt from GitHub Releases (fast path)
$ok = $false
try {
    $tmp = Join-Path $env:TEMP ("screenmate-install-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    $zip = Join-Path $tmp "screenmate.zip"
    Write-Host "Downloading $Asset ..."
    Invoke-WebRequest -Uri $Asset -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $tmp -Force
    $src = Get-ChildItem -Path $tmp -Filter screenmate.exe -Recurse | Select-Object -First 1
    if ($src) {
        New-Item -ItemType Directory -Path $BinDir -Force | Out-Null
        Copy-Item $src.FullName $ExePath -Force
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
        $ok = $true
        Write-Host "Installed: $ExePath" -ForegroundColor Green
    }
} catch {
    Write-Warning "Prebuilt download failed: $_"
}

# 2) fallback: clone repo and build (needs git + cmake + VS Build Tools)
if (-not $ok) {
    Write-Host "Fallback: build from source (git + cmake + Visual Studio Build Tools)..."
    $srcDir = Join-Path $env:TEMP "screenmate-src"
    if (Test-Path $srcDir) { Remove-Item $srcDir -Recurse -Force }
    git clone --depth 1 --branch $Branch "$Repo" $srcDir
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
    Push-Location $srcDir
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed — install Visual Studio Build Tools 2022 + CMake" }
    cmake --build build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    $built = Join-Path $srcDir "build\Release\screenmate.exe"
    if (-not (Test-Path $built)) { throw "screenmate.exe not produced" }
    New-Item -ItemType Directory -Path $BinDir -Force | Out-Null
    Copy-Item $built $ExePath -Force
    Pop-Location
    $ok = $true
    Write-Host "Installed (built from source): $ExePath" -ForegroundColor Green
}

# 3) add to user PATH
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if (-not $userPath) { $userPath = "" }
if ($userPath -notlike "*$BinDir*") {
    [Environment]::SetEnvironmentVariable("Path", ($userPath.TrimEnd(";") + ";$BinDir"), "User")
    Write-Host "Added to PATH: $BinDir (new terminals only)" -ForegroundColor Green
}

Write-Host ""
Write-Host "Done. Open a NEW terminal and run:" -ForegroundColor Cyan
Write-Host "  screenmate            # chat UI" 
Write-Host "  screenmate auth login # set API key"
