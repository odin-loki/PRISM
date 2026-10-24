@echo off
REM Thin wrapper for manual resumes (see docs/MSVC_PORT.md). Target: libz3 or prism_tests.
set BUILD=C:\Temp\prism-build-win-z3
set VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
set CMAKE=C:\Program Files\CMake\bin\cmake.exe
if "%~1"=="" (
  echo Usage: %~nx0 libz3^|prism_tests
  exit /b 1
)
call "%VCVARS%" || exit /b 1
"%CMAKE%" --build "%BUILD%" --target %1 -j 32
exit /b %ERRORLEVEL%
