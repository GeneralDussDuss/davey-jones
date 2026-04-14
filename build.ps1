#
# DAVEY JONES build script (Windows PowerShell).
#
# Auto-detects ESP-IDF installation. Override with environment variables:
#   $env:IDF_PATH       = path to esp-idf checkout (e.g. C:\Espressif\frameworks\esp-idf-v5.3.2)
#   $env:IDF_TOOLS_PATH = ESP-IDF tools root       (e.g. C:\Espressif)
#
# Why this script exists:
#   Git Bash on Windows exports MSYS/MinGW env vars that break the ESP-IDF
#   toolchain. This script scrubs those, then calls export.ps1 from the
#   detected IDF install so `idf.py` resolves properly.
#

Write-Host "=== DAVEY JONES BUILD ===" -ForegroundColor Magenta

# Scrub MSYS/MinGW env vars inherited from Git Bash.
"MSYSTEM","MSYSTEM_CARCH","MSYSTEM_CHOST","MSYSTEM_PREFIX",
"MINGW_PREFIX","MINGW_CHOST","MINGW_PACKAGE_PREFIX","MSYS",
"CHERE_INVOKING" | ForEach-Object {
    Remove-Item "Env:$_" -ErrorAction SilentlyContinue
}

# ---- Auto-detect IDF_PATH if not set ----
if (-not $env:IDF_PATH) {
    $candidates = @(
        "C:\Espressif\frameworks",
        "$HOME\esp",
        "$HOME\Documents\esp"
    )
    foreach ($root in $candidates) {
        if (Test-Path $root) {
            $found = Get-ChildItem $root -Directory -Filter "esp-idf-v*" -ErrorAction SilentlyContinue |
                     Sort-Object Name -Descending | Select-Object -First 1
            if ($found) { $env:IDF_PATH = $found.FullName; break }
            $found = Get-ChildItem $root -Directory -Filter "esp-idf" -ErrorAction SilentlyContinue |
                     Select-Object -First 1
            if ($found) { $env:IDF_PATH = $found.FullName; break }
        }
    }
}

if (-not $env:IDF_PATH -or -not (Test-Path $env:IDF_PATH)) {
    Write-Host "ERROR: ESP-IDF not found. Set \$env:IDF_PATH to your ESP-IDF install." -ForegroundColor Red
    Write-Host "  example: `$env:IDF_PATH = 'C:\Espressif\frameworks\esp-idf-v5.3.2'" -ForegroundColor Yellow
    exit 1
}

# ---- Auto-detect IDF_TOOLS_PATH if not set ----
if (-not $env:IDF_TOOLS_PATH) {
    # If IDF_PATH is under C:\Espressif\frameworks\..., tools are at C:\Espressif
    if ($env:IDF_PATH -match "^(.+)\\frameworks\\") {
        $env:IDF_TOOLS_PATH = $matches[1]
    } elseif (Test-Path "$HOME\.espressif") {
        $env:IDF_TOOLS_PATH = "$HOME\.espressif"
    } else {
        $env:IDF_TOOLS_PATH = "C:\Espressif"
    }
}

Write-Host "IDF_PATH:       $env:IDF_PATH"
Write-Host "IDF_TOOLS_PATH: $env:IDF_TOOLS_PATH"

# ---- Find IDF Python venv (must be BEFORE idf-python in PATH). ----
# Prepend idf-python first, then venv on top, so venv python wins.
$idfToolsPython = Get-ChildItem "$env:IDF_TOOLS_PATH\tools\idf-python" -Directory -ErrorAction SilentlyContinue |
                  Sort-Object Name -Descending | Select-Object -First 1
if ($idfToolsPython) {
    $env:PATH = "$($idfToolsPython.FullName);$env:PATH"
}

$venvScripts = Get-ChildItem "$env:IDF_TOOLS_PATH\python_env" -Directory -ErrorAction SilentlyContinue |
               Sort-Object Name -Descending | Select-Object -First 1
if ($venvScripts) {
    $env:PATH = "$($venvScripts.FullName)\Scripts;$env:PATH"
}

Write-Host "--- Initializing ESP-IDF ---"
$exportScript = Join-Path $env:IDF_PATH "export.ps1"
if (-not (Test-Path $exportScript)) {
    Write-Host "ERROR: export.ps1 not found at $exportScript" -ForegroundColor Red
    exit 1
}
& $exportScript

Write-Host ""
Write-Host "--- Python ---"
& where.exe python | Select-Object -First 2
python --version

Write-Host ""
Write-Host "--- Building Davey Jones ---"
$projectDir = Split-Path -Parent $PSCommandPath
Set-Location $projectDir

$idfpy = Join-Path $env:IDF_PATH "tools\idf.py"
python $idfpy set-target esp32c6
python $idfpy build

Write-Host ""
Write-Host "=== DONE (exit: $LASTEXITCODE) ===" -ForegroundColor Magenta
if ($LASTEXITCODE -eq 0) {
    Write-Host "To flash:  idf.py -p COMx flash" -ForegroundColor Green
}
