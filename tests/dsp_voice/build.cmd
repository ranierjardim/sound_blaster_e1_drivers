@echo off
rem Build and run the libe1dsp voice-module tests (owner: dsp-voice).
rem Usage: build.cmd   (from any directory; needs MSVC 2022 BuildTools + Windows SDK)
rem Outputs go to tests\dsp_voice\build\. Exit code 0 = all tests passed.
setlocal
set "HERE=%~dp0"
set "SRC=%HERE%..\..\src\dsp"
set "OUT=%HERE%build"
if not defined VCToolsInstallDir call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%"
set "CFLAGS=/nologo /W4 /WX /O2 /fp:precise /std:c11 /MT /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" /I"%HERE%..\..\include""
cl %CFLAGS% "%HERE%test_voice.c" "%SRC%\e1_fft.c" "%SRC%\e1_nr.c" "%SRC%\e1_voicefx.c" /Fe:"%OUT%\test_voice.exe"
if errorlevel 1 ( echo BUILD FAILED: test_voice & exit /b 1 )
cl %CFLAGS% "%HERE%test_nr_whitebox.c" "%SRC%\e1_fft.c" /Fe:"%OUT%\test_nr_whitebox.exe"
if errorlevel 1 ( echo BUILD FAILED: test_nr_whitebox & exit /b 1 )
set RC=0
"%OUT%\test_voice.exe"
if errorlevel 1 set RC=1
echo.
"%OUT%\test_nr_whitebox.exe"
if errorlevel 1 set RC=1
echo.
if %RC%==0 (echo ALL VOICE TESTS PASSED) else (echo VOICE TESTS FAILED)
exit /b %RC%
