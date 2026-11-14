@echo off
rem Unit tests: build tests\tests.exe and run it. Returns 1 on failure.
rem Only pure-logic modules are linked (sourcejson.cpp / update.cpp) - no window, no network.
rem BIN can be overridden by an environment variable (llvm-mingw may live elsewhere in CI).
setlocal
cd /d "%~dp0.."

if "%BIN%"=="" set BIN=D:\llvm-mingw\bin
if not exist "%BIN%\g++.exe" (
  echo [ERROR] compiler not found: %BIN%\g++.exe
  exit /b 1
)

if not exist tests mkdir tests

echo === build tests ===
"%BIN%\g++.exe" -std=gnu++17 -O1 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -mconsole -Wall -Wextra -o tests\tests.exe tests\tests.cpp src\sourcejson.cpp src\update.cpp -static -static-libgcc -static-libstdc++ -luser32 -lkernel32
if errorlevel 1 goto fail

echo === run tests ===
tests\tests.exe
if errorlevel 1 goto fail

echo === tests OK ===
endlocal
exit /b 0

:fail
echo.
echo [FAILED] tests
endlocal
exit /b 1
