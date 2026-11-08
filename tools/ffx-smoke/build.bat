@echo off
rem ffx-smoke -- standalone proof that FSR 4.1 initializes on this GPU.
rem Output (exe + the AMD DLLs it loads at runtime) land in build\ffx-smoke\.
rem Prerequisite: deploy\fetch-deps.ps1 once.
cd /d "%~dp0"
setlocal
call "%~dp0..\vcvars.bat" x64 || exit /b 1
set "ROOT=%~dp0..\.."
set "FFXBIN=%ROOT%\deploy\ffx-sdk\FidelityFX-SDK-2.4.0\Kits\FidelityFX\signedbin"
if not exist "%FFXBIN%\amd_fidelityfx_loader_dx12.dll" (
    echo [ffx-smoke] FidelityFX DLLs not found -- run: powershell -File deploy\fetch-deps.ps1
    exit /b 1
)
if not exist "%ROOT%\build\ffx-smoke" mkdir "%ROOT%\build\ffx-smoke"
cl /nologo /O2 /EHsc /W4 /MD /D_WINDOWS ^
   /I"%ROOT%\external\ffx\api\include" /I"%ROOT%\external\ffx\upscalers\include" ^
   ffx_smoke.cpp /Fe:"%ROOT%\build\ffx-smoke\ffx_smoke.exe" ^
   /Fo:"%ROOT%\build\ffx-smoke\ffx_smoke.obj" ^
   /link d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
copy /y "%FFXBIN%\amd_fidelityfx_loader_dx12.dll"   "%ROOT%\build\ffx-smoke\" >nul
copy /y "%FFXBIN%\amd_fidelityfx_upscaler_dx12.dll" "%ROOT%\build\ffx-smoke\" >nul
endlocal
echo ffx-smoke built into build\ffx-smoke\.
