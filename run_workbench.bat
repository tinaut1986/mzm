@echo off
setlocal
set "SCRIPT_DIR=%~dp0"

where python >nul 2>nul
if %ERRORLEVEL% equ 0 (
    set "PYTHON_BIN=python"
    goto :run
)

where py >nul 2>nul
if %ERRORLEVEL% equ 0 (
    set "PYTHON_BIN=py -3"
    goto :run
)

where python3 >nul 2>nul
if %ERRORLEVEL% equ 0 (
    set "PYTHON_BIN=python3"
    goto :run
)

echo Error: No se encontro Python instalado en el sistema.
pause
exit /b 1

:run
echo Iniciando Layer Workbench...
%PYTHON_BIN% "%SCRIPT_DIR%tools\layer-workbench\serve.py" %*
