@echo off
echo === DAVEY JONES BUILD ===

set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.3.2
set IDF_TOOLS_PATH=C:\Espressif
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.3_py3.11_env

:: Activate the IDF Python venv
call "%IDF_PYTHON_ENV_PATH%\Scripts\activate.bat"

:: Add IDF tools to PATH
set PATH=C:\Espressif\tools\idf-python\3.11.2;%PATH%
set PATH=C:\Espressif\tools\idf-git\2.44.0\cmd;%PATH%

:: Run export.bat to get cmake/ninja/toolchain in PATH
call "%IDF_PATH%\export.bat"

echo.
echo --- idf.py version ---
python "%IDF_PATH%\tools\idf.py" --version

echo.
echo --- Setting target ---
cd /d C:\Users\D\davey_jones
python "%IDF_PATH%\tools\idf.py" set-target esp32c6

echo.
echo --- Building ---
python "%IDF_PATH%\tools\idf.py" build 2>&1

echo.
echo === DONE (exit: %ERRORLEVEL%) ===
pause
