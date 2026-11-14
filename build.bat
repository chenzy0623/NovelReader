@echo off
rem =========================================================
rem  Build NovelReader.exe  (LLVM-MinGW / clang + windres)
rem  Keep this file pure ASCII: cmd.exe reads .bat in the OEM
rem  code page, so non-ASCII characters break the parser.
rem  BIN can be overridden by an environment variable (CI).
rem  Set NR_NOPAUSE=1 to skip the final pause (CI / scripts).
rem =========================================================
setlocal
cd /d "%~dp0"

if "%BIN%"=="" set BIN=D:\llvm-mingw\bin
if not exist "%BIN%\g++.exe" (
  echo [ERROR] compiler not found: %BIN%\g++.exe
  echo         set BIN to the llvm-mingw "bin" directory and retry.
  if "%NR_NOPAUSE%"=="" pause
  exit /b 1
)
if not exist build mkdir build

echo === windres ===
"%BIN%\windres.exe" -I src -I res -O coff res\app.rc build\app.res
if errorlevel 1 goto fail

echo === compile + link ===
"%BIN%\g++.exe" -std=gnu++17 -O2 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 ^
  -municode -mwindows -ffunction-sections -fdata-sections -Wall -Wextra ^
  -o build\NovelReader.exe ^
  src\book.cpp src\ini.cpp src\layout.cpp src\view.cpp src\ui.cpp src\zip.cpp src\format.cpp src\pdf.cpp src\pdfrender.cpp src\server.cpp src\web.cpp src\source.cpp src\sourcejson.cpp src\update.cpp src\crashdump.cpp src\sync.cpp build\app.res ^
  "-Wl,--gc-sections" "-Wl,-s" ^
  -static -static-libgcc -static-libstdc++ ^
  -lgdiplus -lshcore -lruntimeobject -lwininet -lws2_32 -liphlpapi -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lgdi32 -luser32 -ladvapi32 -ldbghelp
if errorlevel 1 goto fail

echo.
for %%F in (build\NovelReader.exe) do echo === output: %%~fF  (%%~zF bytes) ===
echo.
echo Build OK: build\NovelReader.exe
set ERR=0
goto end

:fail
echo.
echo [FAILED] build error, see the messages above.
set ERR=1
:end
if "%NR_NOPAUSE%"=="" pause
endlocal & exit /b %ERR%
