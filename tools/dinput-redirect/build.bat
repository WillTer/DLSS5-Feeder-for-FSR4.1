@echo off
rem dinput8.dll proxy for SpellForce (32-bit): diverts every legacy DirectX DLL
rem load to the dgVoodoo set in the game folder. See dinput8_redirect.cpp.
rem Output goes to build\ -- install.bat places it into the game folder.
cd /d "%~dp0"
setlocal
call "%~dp0..\vcvars.bat" amd64_x86 || exit /b 1
set "ROOT=%~dp0..\.."
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
cl /nologo /LD /O2 /W4 /MT /I"%ROOT%\external\minhook\include" ^
   dinput8_redirect.cpp ^
   "%ROOT%\external\minhook\src\buffer.c" "%ROOT%\external\minhook\src\hook.c" ^
   "%ROOT%\external\minhook\src\trampoline.c" "%ROOT%\external\minhook\src\hde\hde32.c" ^
   /Fe:"%ROOT%\build\dinput8.dll" ^
   /link /DEF:dinput8.def kernel32.lib user32.lib
if errorlevel 1 exit /b 1
endlocal
echo dinput8 proxy built into build\.
