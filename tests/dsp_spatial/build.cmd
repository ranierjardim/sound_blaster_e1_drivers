@echo off
rem Build and run the dsp-spatial unit tests (Surround, Clarity, Dialog+, Smart Volume).
rem   build.cmd            build + run full suite (incl. 60 s performance runs)
rem   build.cmd quick      build + run shortened suite
rem   build.cmd norun      build only
rem Output goes to tests\dsp_spatial\bin\.  Needs MSVC 2022 (vcvars64 is called if cl is not on PATH).
setlocal EnableExtensions
set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
set "SRC=%ROOT%\src\dsp"
set "OUT=%HERE%bin"
if not exist "%OUT%" mkdir "%OUT%"

where cl >nul 2>nul
if errorlevel 1 call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul
if errorlevel 1 (
  echo cl.exe not found - install MSVC 2022 Build Tools
  exit /b 2
)

rem DSP flags: strict IEEE (/fp:precise), x64 SSE2 baseline (no /arch), C11, warnings as errors.
cl /nologo /std:c11 /W4 /WX /O2 /fp:precise /I"%SRC%" /I"%ROOT%\include" ^
   /Fo"%OUT%\\" /Fe"%OUT%\test_spatial.exe" ^
   "%HERE%test_spatial.c" ^
   "%SRC%\e1_surround.c" "%SRC%\e1_hrtf_data.c" "%SRC%\e1_crystal.c" "%SRC%\e1_dialog.c" "%SRC%\e1_svm.c"
if errorlevel 1 exit /b 1

if /i "%~1"=="norun" exit /b 0
"%OUT%\test_spatial.exe" %1
exit /b %errorlevel%
