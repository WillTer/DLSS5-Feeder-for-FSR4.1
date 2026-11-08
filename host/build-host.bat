@echo off
rem dlss5-feed-host64.exe -- the 64-bit FSR 4.1 host for 32-bit games.
rem Output (and the AMD DLLs it needs at runtime) land in build\ -- the source
rem tree stays binary-free; install.bat places them into the game folder.
rem Prerequisite: deploy\fetch-deps.ps1 once (FidelityFX SDK headers + DLLs).
cd /d "%~dp0"
setlocal
call "%~dp0..\tools\vcvars.bat" x64 || exit /b 1
set "ROOT=%~dp0.."
set "FFXBIN=%ROOT%\deploy\ffx-sdk\FidelityFX-SDK-2.4.0\Kits\FidelityFX\signedbin"
if not exist "%FFXBIN%\amd_fidelityfx_loader_dx12.dll" (
    echo [host] FidelityFX DLLs not found -- run: powershell -File deploy\fetch-deps.ps1
    exit /b 1
)
for /f %%I in ('git rev-parse --short HEAD 2^>nul') do set FEED_BUILD_ID=%%I
if not defined FEED_BUILD_ID set FEED_BUILD_ID=fsr41
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
cl /nologo /O2 /EHsc /W3 /MD /D_WINDOWS /DFEED_BUILD_ID=\"%FEED_BUILD_ID%\" ^
   /I"%ROOT%\external\ffx\api\include" /I"%ROOT%\external\ffx\upscalers\include" ^
   dlss5-feed-host64.cpp ^
   /Fe:"%ROOT%\build\dlss5-feed-host64.exe" ^
   /Fo:"%ROOT%\build\dlss5-feed-host64.obj" ^
   /link d3d12.lib dxgi.lib version.lib winmm.lib kernel32.lib user32.lib gdi32.lib advapi32.lib ole32.lib
if errorlevel 1 exit /b 1
copy /y "%FFXBIN%\amd_fidelityfx_loader_dx12.dll"   "%ROOT%\build\" >nul
copy /y "%FFXBIN%\amd_fidelityfx_upscaler_dx12.dll" "%ROOT%\build\" >nul
endlocal
echo host built into build\ (with the AMD runtime DLLs beside it for --test).
