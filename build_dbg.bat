@echo off
rem =====================================================================
rem  Build debug exe with trace log (NovelReader_dbg.exe + nr_trace.log)
rem  ASCII only on purpose: this .bat stays valid in any code page.
rem =====================================================================
setlocal
cd /d "%~dp0"
set BIN=D:\llvm-mingw\bin
if not exist build mkdir build

"%BIN%\windres.exe" -I src -I res -O coff res\app.rc build\app_dbg.res
if errorlevel 1 goto fail

"%BIN%\g++.exe" -std=gnu++17 -O1 -g -DNR_TRACE -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 ^
  -municode -mwindows -Wall -Wextra ^
  -o build\NovelReader_dbg.exe ^
  src\book.cpp src\ini.cpp src\view.cpp src\ui.cpp src\zip.cpp src\format.cpp src\pdf.cpp src\server.cpp src\trace.cpp build\app_dbg.res ^
  -static -static-libgcc -static-libstdc++ ^
  -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lgdi32 -luser32 -ladvapi32
if errorlevel 1 goto fail

echo.
echo OK: build\NovelReader_dbg.exe
goto end

:fail
echo.
echo BUILD FAILED
:end
endlocal
exit /b %errorlevel%
