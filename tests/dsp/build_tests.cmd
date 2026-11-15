@echo off
rem ---------------------------------------------------------------------------------------
rem build_tests.cmd - builds libe1dsp + the C unit tests + e1wavfx.exe, then runs the tests.
rem
rem   build_tests.cmd [stubs] [norun]
rem     stubs : link the stub versions of every module owned by the other DSP implementers
rem             (surround, crystal, dialog, svm, nr, voicefx) instead of the real sources.
rem     norun : build only.
rem   Environment:
rem     E1_STUB_DIR  directory holding stub_<module>.c files; used for "stubs" mode and for any
rem                  module whose real source does not exist yet in src\dsp.
rem     E1_BUILD_DIR output directory (default: tests\dsp\build).
rem   Flags: x64, C11, /W4 /O2 /fp:precise, SSE2 baseline (no /arch:AVX2), static CRT.
rem ---------------------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion
set "HERE=%~dp0"
for %%I in ("%HERE%..\..") do set "ROOT=%%~fI"
set "DSP=%ROOT%\src\dsp"
if not defined E1_BUILD_DIR set "E1_BUILD_DIR=%HERE%build"
set "OUT=%E1_BUILD_DIR%"
set "MODE=real"
set "RUN=1"
for %%A in (%*) do (
    if /I "%%~A"=="stubs" set "MODE=stubs"
    if /I "%%~A"=="norun" set "RUN=0"
)

where cl >nul 2>nul
if errorlevel 1 (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
    if errorlevel 1 (
        echo [build] vcvars64.bat failed
        exit /b 1
    )
)

if not exist "%OUT%\obj" mkdir "%OUT%\obj"
del /q "%OUT%\obj\*.obj" >nul 2>nul
set "CFLAGS=/nologo /std:c11 /W4 /O2 /fp:precise /MT /D_CRT_SECURE_NO_WARNINGS"

rem --- library sources -------------------------------------------------------------------
set "SRCS="
for %%F in (e1fx_params e1_eq e1_bass e1_limiter e1dsp_chain) do set SRCS=!SRCS! "%DSP%\%%F.c"
if /I "%MODE%"=="real" (
    for %%F in ("%DSP%\*.c") do (
        set "IS_CORE=0"
        for %%C in (e1fx_params e1_eq e1_bass e1_limiter e1dsp_chain) do if /I "%%~nF"=="%%C" set "IS_CORE=1"
        if !IS_CORE!==0 set SRCS=!SRCS! "%%~fF"
    )
)
set "STUBBED="
for %%M in (surround crystal dialog svm nr voicefx) do (
    set "NEED=1"
    if /I "%MODE%"=="real" if exist "%DSP%\e1_%%M.c" set "NEED=0"
    if !NEED!==1 (
        if not defined E1_STUB_DIR (
            echo [build] src\dsp\e1_%%M.c is missing and E1_STUB_DIR is not set
            exit /b 1
        )
        set SRCS=!SRCS! "%E1_STUB_DIR%\stub_%%M.c"
        set "STUBBED=!STUBBED! %%M"
    )
)
if defined STUBBED (echo [build] mode %MODE%, stubbed modules:%STUBBED%) else (echo [build] mode %MODE%, all real modules)

pushd "%OUT%\obj"
cl %CFLAGS% /c /I"%DSP%" %SRCS%
if errorlevel 1 (
    popd
    echo [build] library compile failed
    exit /b 1
)
popd

rem --- tests and tools --------------------------------------------------------------------
set "TESTS=test_params test_eq test_bass test_limiter test_chain test_perf"
for %%T in (%TESTS%) do (
    cl %CFLAGS% /I"%DSP%" /Fe"%OUT%\%%T.exe" /Fo"%OUT%\%%T.obj" "%HERE%%%T.c" "%OUT%\obj\*.obj" >"%OUT%\%%T.buildlog" 2>&1
    if errorlevel 1 (
        type "%OUT%\%%T.buildlog"
        echo [build] %%T failed to build
        exit /b 1
    )
    findstr /I /C:" warning " "%OUT%\%%T.buildlog"
)
cl %CFLAGS% /I"%DSP%" /Fe"%OUT%\e1wavfx.exe" /Fo"%OUT%\\" "%HERE%e1wavfx.c" "%HERE%wav.c" "%OUT%\obj\*.obj" >"%OUT%\e1wavfx.buildlog" 2>&1
if errorlevel 1 (
    type "%OUT%\e1wavfx.buildlog"
    echo [build] e1wavfx failed to build
    exit /b 1
)
findstr /I /C:" warning " "%OUT%\e1wavfx.buildlog"
echo [build] OK: %OUT%

if "%RUN%"=="0" exit /b 0
set "FAILS=0"
for %%T in (%TESTS%) do (
    "%OUT%\%%T.exe"
    if errorlevel 1 set /a FAILS+=1
)
echo ==== test programs failed: !FAILS!
if !FAILS! neq 0 exit /b 1
exit /b 0
