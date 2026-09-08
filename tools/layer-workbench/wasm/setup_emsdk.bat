@echo off
REM Bootstrap a repo-local Emscripten toolchain under wasm\emsdk\ so the layer
REM workbench can compile depth_engine.wasm without a system-wide install.
REM
REM   tools\layer-workbench\wasm\setup_emsdk.bat
REM
REM Idempotent: clones emsdk on first run, then installs + activates "latest".
REM run_workbench.bat calls this automatically when emcc is not already on PATH.
REM The emsdk\ tree is git-ignored.
setlocal
set "HERE=%~dp0"
set "EMSDK_DIR=%HERE%emsdk"

where git >nul 2>nul
if errorlevel 1 (
    echo Error: git no esta en el PATH.
    exit /b 1
)

if not exist "%EMSDK_DIR%\emsdk.bat" (
    echo Clonando emsdk en %EMSDK_DIR% ...
    git clone https://github.com/emscripten-core/emsdk.git "%EMSDK_DIR%"
    if errorlevel 1 exit /b 1
)

pushd "%EMSDK_DIR%"
call emsdk.bat install latest
if errorlevel 1 ( popd & exit /b 1 )
call emsdk.bat activate latest
if errorlevel 1 ( popd & exit /b 1 )
popd

echo.
echo emsdk local listo. run_workbench.bat lo usara automaticamente.
endlocal
