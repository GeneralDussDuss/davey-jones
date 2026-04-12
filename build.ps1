Write-Host "=== DAVEY JONES BUILD ===" -ForegroundColor Magenta

# Kill MSYS/MinGW env vars inherited from Git Bash
"MSYSTEM","MSYSTEM_CARCH","MSYSTEM_CHOST","MSYSTEM_PREFIX",
"MINGW_PREFIX","MINGW_CHOST","MINGW_PACKAGE_PREFIX","MSYS",
"CHERE_INVOKING" | ForEach-Object {
    Remove-Item "Env:$_" -ErrorAction SilentlyContinue
}

$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.3.2"
$env:IDF_TOOLS_PATH = "C:\Espressif"

# Force IDF's venv Python FIRST so `python` resolves to the venv
$env:PATH = "C:\Espressif\python_env\idf5.3_py3.11_env\Scripts;C:\Espressif\tools\idf-python\3.11.2;$env:PATH"

Write-Host "--- Initializing ESP-IDF ---"
& "$env:IDF_PATH\export.ps1"

Write-Host ""
Write-Host "--- Which python? ---"
& where.exe python | Select-Object -First 2
python --version

Write-Host ""
Write-Host "--- Building Davey Jones ---"
Set-Location C:\Users\D\davey_jones

# Use the venv python directly to call idf.py
$idfpy = "$env:IDF_PATH\tools\idf.py"
python $idfpy set-target esp32c6
python $idfpy build

Write-Host "`n=== DONE (exit: $LASTEXITCODE) ===" -ForegroundColor Magenta
