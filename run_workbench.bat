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

REM --- WASM depth engine: aprovisiona un Emscripten local la primera vez ------
REM serve.py ya reconstruye depth_engine.js cuando emcc esta disponible; aqui
REM solo nos aseguramos de que lo este. Un emcc del sistema tiene prioridad.
set "WASM_DIR=%SCRIPT_DIR%tools\layer-workbench\wasm"
where emcc >nul 2>nul
if %ERRORLEVEL% equ 0 goto :launch
if exist "%WASM_DIR%\emsdk\upstream\emscripten\emcc.bat" goto :launch
echo.
echo El motor de profundidad WASM necesita Emscripten y no se encuentra.
echo Puedo instalarlo dentro del repo (tools\layer-workbench\wasm\emsdk, ~1 GB).
choice /c SN /n /m "Instalar ahora? [S/N] "
if errorlevel 2 goto :launch
call "%WASM_DIR%\setup_emsdk.bat"

:launch
echo Iniciando Layer Workbench...
%PYTHON_BIN% "%SCRIPT_DIR%tools\layer-workbench\serve.py" %*
