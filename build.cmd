@echo off
rem ===========================================================================================
rem  E1 Studio - top-level build script (MSVC 2022 + Windows SDK, x64, no WDK).
rem
rem    build.cmd [release|debug] [test] [full] [clean] [nowx] [nops]
rem
rem      release  (default) optimised build      -> build\x64\Release\
rem      debug    unoptimised build with checks   -> build\x64\Debug\
rem      test     build, then run every test program and the PowerShell self-check
rem      full     with "test": run the long variants too (test_spatial without "quick")
rem      clean    delete the object and output folders of the selected configuration first
rem      nowx     do not treat warnings as errors (emergency only)
rem      nops     skip the PowerShell self-check in "test"
rem
rem  Components (compiled by wildcard, so new .c files are picked up automatically):
rem    src\dsp\*.c      -> libe1dsp.lib   pure C DSP, /fp:precise, no OS headers
rem    src\common\*.c   -> e1common.lib   IPC helpers (Windows)
rem    src\apo\*.c      -> e1fx.dll       APO (+ src\apo\*.def, src\apo\*.rc)
rem    src\panel\*.c    -> e1panel.exe    control panel (+ src\panel\*.rc, *.manifest)
rem    tools\*.c        -> e1ctl.exe      CLI
rem    tests\<dir>\, tools\tests\        -> test programs (see "Test conventions")
rem  Generated resources: when src\panel has no e1panel.ico, tests\panel\mkicon.c (renders our own
rem  emblem through src\panel\gfx.c) is built and run to create it before the panel's .rc.
rem  A component directory may contain "link.libs": extra import libraries, whitespace
rem  separated, '#' starts a comment line.
rem  Environment: E1_SKIP="panel apo ..." skips components still under construction (names:
rem  common apo panel e1ctl tests); the tests of a skipped component (tests\<name>) are skipped
rem  too; skipped components are reported and make "test" fail.
rem
rem  Test conventions (tests\<dir>\ and tools\tests\):
rem    * every test_*.c is one test program;
rem    * a non-test .c that defines main/wmain/WinMain is a tool: built, not run (e.g. e1wavfx);
rem    * the remaining .c files are helpers, linked into every program of the directory;
rem    * a directory with no test_*.c builds ONE test program test_<dir>.exe from all its .c;
rem    * programs also link libe1dsp.lib, e1common.lib and a static library of the component
rem      with the same name (tests\apo -> src\apo, tests\panel -> src\panel, tools\tests ->
rem      tools), so only the objects a test references are pulled in;
rem    * arguments: the first line of <testdir>\<program>.args if that file exists;
rem      test_spatial.exe gets "quick" unless "full" is given;
rem    * a test passes when it exits with 0. Tests run in build\x64\<cfg>\ (next to e1fx.dll and
rem      e1ctl.exe) with E1_ROOT (repository root) and E1_TEST_SRC (test source dir) set.
rem ===========================================================================================
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "CONFIG=Release"
set "DO_TEST=0"
set "DO_FULL=0"
set "DO_CLEAN=0"
set "DO_PS=1"
set "WX=/WX"

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="release" ( set "CONFIG=Release" & shift & goto parse_args )
if /i "%~1"=="debug"   ( set "CONFIG=Debug"   & shift & goto parse_args )
if /i "%~1"=="test"    ( set "DO_TEST=1"      & shift & goto parse_args )
if /i "%~1"=="full"    ( set "DO_FULL=1"      & shift & goto parse_args )
if /i "%~1"=="clean"   ( set "DO_CLEAN=1"     & shift & goto parse_args )
if /i "%~1"=="nowx"    ( set "WX="            & shift & goto parse_args )
if /i "%~1"=="nops"    ( set "DO_PS=0"        & shift & goto parse_args )
if /i "%~1"=="help"    goto usage
if /i "%~1"=="/?"      goto usage
echo build.cmd: unknown argument "%~1"
goto usage_fail
:args_done

rem ---- toolchain ------------------------------------------------------------------------------
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" goto find_vcvars
where cl.exe >nul 2>nul && goto have_toolchain
:find_vcvars
set "VCVARS="
set "VSINST="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto vcvars_fallback
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINST=%%I"
if not defined VSINST goto vcvars_fallback
if exist "%VSINST%\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VSINST%\VC\Auxiliary\Build\vcvars64.bat"
:vcvars_fallback
if not defined VCVARS set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" goto call_vcvars
echo build.cmd: MSVC x64 toolchain not found. Install Visual Studio 2022 Build Tools with the C++ workload.
exit /b 1
:call_vcvars
rem vcvars looks for vswhere.exe on PATH; give it the installer folder to avoid a spurious error
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VCVARS%" >nul
if errorlevel 1 goto vcvars_failed
where cl.exe >nul 2>nul && goto have_toolchain
:vcvars_failed
echo build.cmd: vcvars64.bat did not set up cl.exe
exit /b 1
:have_toolchain

rem ---- paths and flags ----------------------------------------------------------------------
set "OUT=%ROOT%\build\x64\%CONFIG%"
set "OBJ=%ROOT%\build\obj\x64\%CONFIG%"

if not "%DO_CLEAN%"=="1" goto after_clean
echo === clean %CONFIG%
if exist "%OBJ%\" rmdir /s /q "%OBJ%"
if exist "%OUT%\" rmdir /s /q "%OUT%"
:after_clean
if not exist "%OUT%\" mkdir "%OUT%"
if not exist "%OBJ%\" mkdir "%OBJ%"

rem /fp:precise everywhere (never /fp:fast: the DSP relies on IEEE semantics); the x64 baseline
rem is SSE2, so no /arch switch is given. Static CRT (/MT) so e1fx.dll loads into audiodg.exe
rem without a Visual C++ redistributable.
set CFLAGS=/nologo /c /std:c11 /experimental:c11atomics /W4 %WX% /utf-8 /fp:precise /Zi /FS /MP /GS /guard:cf /Gy /Gw ^
 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ^
 /I"%ROOT%\include" /I"%ROOT%\src\dsp" /I"%ROOT%\src\common"
set LFLAGS=/nologo /DEBUG /MACHINE:X64 /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /GUARD:CF /INCREMENTAL:NO
if /i "%CONFIG%"=="Release" (
    set CFLAGS=!CFLAGS! /O2 /MT /DNDEBUG
    set LFLAGS=!LFLAGS! /OPT:REF /OPT:ICF
) else (
    set CFLAGS=!CFLAGS! /Od /MTd /D_DEBUG /RTC1
)
if not defined WX echo build.cmd: WARNING - warnings are NOT treated as errors (nowx)

set "WINLIBS=kernel32.lib user32.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib shell32.lib"
set "APOLIBS=%WINLIBS% avrt.lib propsys.lib"
set "PANELLIBS=gdi32.lib gdiplus.lib d2d1.lib dwrite.lib windowscodecs.lib comctl32.lib comdlg32.lib shlwapi.lib uxtheme.lib dwmapi.lib msimg32.lib winmm.lib hid.lib setupapi.lib"
set "CTLLIBS=%WINLIBS%"

set /a NBUILT=0
set /a NSKIP=0
set /a NSKIPENV=0
set "C_EXTRA="
set "RC_EXTRA="

echo === E1 Studio build: %CONFIG% x64  (root: %ROOT%)

rem ---- 1. libe1dsp.lib ----------------------------------------------------------------------
call :compile dsp "%ROOT%\src\dsp" || goto fail
if "%COMPILED%"=="1" goto dsp_ok
echo build.cmd: src\dsp has no .c files - libe1dsp.lib is required by every other component.
goto fail
:dsp_ok
call :mklib dsp libe1dsp.lib || goto fail
set DSPLIB="%OBJ%\libe1dsp.lib"

rem ---- 2. e1common.lib ----------------------------------------------------------------------
set "COMMONLIB="
call :skipped common
if "%SKIPPED%"=="1" ( echo --- SKIPPED by E1_SKIP: e1common.lib& set /a NSKIPENV+=1 & goto after_common )
call :compile common "%ROOT%\src\common" || goto fail
if "%COMPILED%"=="1" goto common_ok
echo --- skip e1common.lib: src\common has no .c files yet
set /a NSKIP+=1
goto after_common
:common_ok
call :mklib common e1common.lib || goto fail
set COMMONLIB="%OBJ%\e1common.lib"
:after_common

rem ---- 3. e1fx.dll --------------------------------------------------------------------------
call :skipped apo
if "%SKIPPED%"=="1" ( echo --- SKIPPED by E1_SKIP: e1fx.dll& set /a NSKIPENV+=1 & goto after_apo )
call :compile apo "%ROOT%\src\apo" || goto fail
if "%COMPILED%"=="1" goto apo_ok
echo --- skip e1fx.dll: src\apo has no .c files yet
set /a NSKIP+=1
goto after_apo
:apo_ok
if not defined COMMONLIB ( echo build.cmd: e1fx.dll needs src\common & goto fail )
call :complib apo "%ROOT%\src\apo" || goto fail
call :rc apo "%ROOT%\src\apo" || goto fail
set L_EXTRA=/DLL /IMPLIB:"%OBJ%\e1fx.lib"
for %%D in ("%ROOT%\src\apo\*.def") do set L_EXTRA=!L_EXTRA! /DEF:"%%~fD"
call :extralibs "%ROOT%\src\apo"
set L_LIBS=%DSPLIB% %COMMONLIB% %APOLIBS% %EXTRALIBS%
call :link apo e1fx.dll || goto fail
:after_apo

rem ---- 4. e1panel.exe -----------------------------------------------------------------------
call :skipped panel
if "%SKIPPED%"=="1" ( echo --- SKIPPED by E1_SKIP: e1panel.exe& set /a NSKIPENV+=1 & goto after_panel )
call :compile panel "%ROOT%\src\panel" || goto fail
if "%COMPILED%"=="1" goto panel_ok
echo --- skip e1panel.exe: src\panel has no .c files yet
set /a NSKIP+=1
goto after_panel
:panel_ok
if not defined COMMONLIB ( echo build.cmd: e1panel.exe needs src\common & goto fail )
call :complib panel "%ROOT%\src\panel" || goto fail
call :panel_icon || goto fail
set RC_EXTRA=/I"%OBJ%\panel_gen"
call :rc panel "%ROOT%\src\panel" || goto fail
set "RC_EXTRA="
set L_EXTRA=/SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:level='asInvoker'
for %%M in ("%ROOT%\src\panel\*.manifest") do set L_EXTRA=/SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"%%~fM" /MANIFESTUAC:NO
if "%RC_HAS_MANIFEST%"=="1" set L_EXTRA=/SUBSYSTEM:WINDOWS /MANIFEST:NO
call :extralibs "%ROOT%\src\panel"
set L_LIBS=%DSPLIB% %COMMONLIB% %WINLIBS% %PANELLIBS% %EXTRALIBS%
call :link panel e1panel.exe || goto fail
:after_panel

rem ---- 5. e1ctl.exe -------------------------------------------------------------------------
if defined COMMONLIB goto ctl_build
echo --- skip e1ctl.exe: needs src\common\e1fx_ipc.c
set /a NSKIP+=1
goto after_ctl
:ctl_build
call :skipped e1ctl
if "%SKIPPED%"=="1" ( echo --- SKIPPED by E1_SKIP: e1ctl.exe& set /a NSKIPENV+=1 & goto after_ctl )
call :compile e1ctl "%ROOT%\tools" || goto fail
if not "%COMPILED%"=="1" goto after_ctl
call :complib e1ctl "%ROOT%\tools" || goto fail
call :extralibs "%ROOT%\tools"
set L_EXTRA=/SUBSYSTEM:CONSOLE
set L_LIBS=%DSPLIB% %COMMONLIB% %CTLLIBS% %EXTRALIBS%
call :link e1ctl e1ctl.exe || goto fail
:after_ctl

rem ---- 6. test programs ---------------------------------------------------------------------
call :skipped tests
if "%SKIPPED%"=="1" ( echo --- SKIPPED by E1_SKIP: test programs& set /a NSKIPENV+=1 & goto tests_built )
set "TESTLIST=%OBJ%\tests.lst"
type nul > "%TESTLIST%"
type nul > "%OBJ%\testnames.lst"
if not exist "%ROOT%\tests\*.c" goto tests_subdirs
call :build_test_dir "%ROOT%\tests" tests "" || goto fail
:tests_subdirs
if not exist "%ROOT%\tests\" goto tests_tools
for /d %%D in ("%ROOT%\tests\*") do call :build_test_dir "%%~fD" "%%~nxD" "%%~nxD" || goto fail
:tests_tools
if not exist "%ROOT%\tools\tests\" goto tests_built
call :build_test_dir "%ROOT%\tools\tests" tools_tests e1ctl || goto fail
:tests_built

echo.
echo === build OK (%CONFIG%): %NBUILT% artifacts in %OUT%
if %NSKIP% gtr 0 echo     %NSKIP% component(s) skipped because their sources do not exist yet
if %NSKIPENV% gtr 0 echo     %NSKIPENV% component(s) skipped by E1_SKIP=%E1_SKIP%
if "%DO_TEST%"=="1" goto run_tests
exit /b 0

rem ===========================================================================================
:run_tests
echo.
echo === running tests
set /a TPASS=0
set /a TFAIL=0
set "FAILED="
set "E1_ROOT=%ROOT%"
for /f "usebackq tokens=1,* delims=|" %%A in ("%TESTLIST%") do call :run_one "%%~A" "%%~B"
if "%DO_PS%"=="0" goto tests_summary
if not exist "%ROOT%\tools\tests\selfcheck.ps1" goto tests_summary
echo --- selfcheck.ps1 (PowerShell, read-only)
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%ROOT%\tools\tests\selfcheck.ps1" -BuildDir "%OUT%"
set "_RC=%errorlevel%"
if "%_RC%"=="0" ( set /a TPASS+=1 & goto tests_summary )
echo *** FAIL: selfcheck.ps1 (exit %_RC%)
set /a TFAIL+=1
set "FAILED=%FAILED% selfcheck.ps1"
:tests_summary
if %NSKIPENV% gtr 0 ( set /a TFAIL+=1 & set "FAILED=%FAILED% [E1_SKIP=%E1_SKIP%]" )
echo.
echo === tests: %TPASS% passed, %TFAIL% failed
if %TFAIL% equ 0 exit /b 0
echo     failed:%FAILED%
exit /b 1

rem :run_one <exe> <source dir>
:run_one
set "E1_TEST_SRC=%~2"
set "_ARGS="
if exist "%~2\%~n1.args" set /p _ARGS=<"%~2\%~n1.args"
if /i "%~n1"=="test_spatial" if not "%DO_FULL%"=="1" if not defined _ARGS set "_ARGS=quick"
echo --- %~nx1 %_ARGS%
pushd "%OUT%"
"%~1" %_ARGS%
set "_RC=%errorlevel%"
popd
if "%_RC%"=="0" ( set /a TPASS+=1 & exit /b 0 )
echo *** FAIL: %~nx1 (exit %_RC%)
set /a TFAIL+=1
set "FAILED=%FAILED% %~n1"
exit /b 0

rem ===========================================================================================
rem :compile <name> <srcdir>   (extra flags in C_EXTRA) -> %OBJ%\<name>\*.obj ; sets COMPILED=0/1
:compile
set "COMPILED=0"
set "_N=%~1"
set "_S=%~2"
set "_O=%OBJ%\%~1"
if not exist "%_S%\*.c" exit /b 0
if exist "%_O%\" rmdir /s /q "%_O%"
mkdir "%_O%"
echo --- compile %_N%  (%_S%)
pushd "%_S%"
cl %CFLAGS% %C_EXTRA% /Fo"%_O%\\" /Fd"%_O%\%_N%.pdb" *.c
set "_E=%errorlevel%"
popd
if not "%_E%"=="0" ( echo *** compile failed: %_N% & exit /b 1 )
set "COMPILED=1"
exit /b 0

rem :skipped <name>  -> SKIPPED=1 when <name> is listed in E1_SKIP
:skipped
set "SKIPPED=0"
if not defined E1_SKIP exit /b 0
echo  %E1_SKIP% | findstr /i /c:" %~1 " >nul && set "SKIPPED=1"
exit /b 0

rem :has_entry <source.c>  -> ENTRY=1 when the file defines main/wmain/WinMain/wWinMain
:has_entry
set "ENTRY=0"
findstr /r /c:"^int  *w*main *(" /c:"^int  *__cdecl  *w*main *(" /c:"WinMain *(" "%~1" >nul 2>nul && set "ENTRY=1"
exit /b 0

rem :complib <name> <srcdir>   static library of a component's objects minus entry points,
rem                            used by the tests of that component (%OBJ%\<name>_t.lib)
:complib
set "_RSP=%OBJ%\%~1_t.lib.rsp"
type nul > "%_RSP%"
set "_CS=%~2"
set "_CO=%OBJ%\%~1"
for %%F in ("%_CO%\*.obj") do call :complib_one "%%~fF" "%%~nF"
lib /nologo /OUT:"%OBJ%\%~1_t.lib" @"%_RSP%" >nul
if errorlevel 1 ( echo *** lib failed: %~1_t.lib & exit /b 1 )
exit /b 0
:complib_one
call :has_entry "%_CS%\%~2.c"
if "%ENTRY%"=="0" echo "%~1">> "%_RSP%"
exit /b 0

rem :panel_icon   builds tests\panel\mkicon.c against the panel objects and renders
rem               %OBJ%\panel_gen\e1panel.ico (skipped when src\panel\e1panel.ico exists)
:panel_icon
if exist "%ROOT%\src\panel\e1panel.ico" exit /b 0
if not exist "%ROOT%\tests\panel\mkicon.c" exit /b 0
set "_G=%OBJ%\panel_gen"
if not exist "%_G%\" mkdir "%_G%"
echo --- generate e1panel.ico (tests\panel\mkicon.c)
cl %CFLAGS% /I"%ROOT%\src\panel" /Fo"%_G%\mkicon.obj" /Fd"%_G%\mkicon.pdb" "%ROOT%\tests\panel\mkicon.c" >"%_G%\mkicon.log"
if errorlevel 1 ( type "%_G%\mkicon.log" & echo *** compile failed: mkicon & exit /b 1 )
link %LFLAGS% /SUBSYSTEM:CONSOLE /OUT:"%_G%\mkicon.exe" /PDB:"%_G%\mkicon.exe.pdb" "%_G%\mkicon.obj" "%OBJ%\panel_t.lib" %DSPLIB% %COMMONLIB% %WINLIBS% %PANELLIBS% >>"%_G%\mkicon.log"
if errorlevel 1 ( type "%_G%\mkicon.log" & echo *** link failed: mkicon & exit /b 1 )
"%_G%\mkicon.exe" "%_G%\e1panel.ico"
if errorlevel 1 ( echo *** mkicon failed & exit /b 1 )
exit /b 0

rem :rc <name> <srcdir>   compiles every .rc into %OBJ%\<name>\ ; sets RC_HAS_MANIFEST
:rc
set "RC_HAS_MANIFEST=0"
set "_RO=%OBJ%\%~1"
set "_RS=%~2"
for %%R in ("%_RS%\*.rc") do call :rc_one "%%~fR" || exit /b 1
exit /b 0
:rc_one
echo --- rc %~nx1
rc /nologo /I"%_RS%" /I"%ROOT%\include" %RC_EXTRA% /fo"%_RO%\%~n1.res" "%~1"
if errorlevel 1 ( echo *** rc failed: %~nx1 & exit /b 1 )
findstr /i /c:"RT_MANIFEST" "%~1" >nul 2>nul && set "RC_HAS_MANIFEST=1"
exit /b 0

rem :extralibs <dir>   reads <dir>\link.libs into EXTRALIBS
:extralibs
set "EXTRALIBS="
if not exist "%~1\link.libs" exit /b 0
for /f "usebackq eol=# tokens=*" %%L in ("%~1\link.libs") do set "EXTRALIBS=!EXTRALIBS! %%L"
exit /b 0

rem :mklib <name> <lib>
:mklib
set "_RSP=%OBJ%\%~1.lib.rsp"
type nul > "%_RSP%"
for %%F in ("%OBJ%\%~1\*.obj") do echo "%%~fF">> "%_RSP%"
lib /nologo /OUT:"%OBJ%\%~2" @"%_RSP%"
if errorlevel 1 ( echo *** lib failed: %~2 & exit /b 1 )
copy /y "%OBJ%\%~2" "%OUT%\%~2" >nul
set /a NBUILT+=1
echo     -^> %OUT%\%~2
exit /b 0

rem :link <name> <output>   (extra flags in L_EXTRA, libraries in L_LIBS)
:link
set "_RSP=%OBJ%\%~1.link.rsp"
type nul > "%_RSP%"
for %%F in ("%OBJ%\%~1\*.obj" "%OBJ%\%~1\*.res") do echo "%%~fF">> "%_RSP%"
link %LFLAGS% %L_EXTRA% /OUT:"%OUT%\%~2" /PDB:"%OUT%\%~n2.pdb" @"%_RSP%" %L_LIBS%
if errorlevel 1 ( echo *** link failed: %~2 & exit /b 1 )
set /a NBUILT+=1
echo     -^> %OUT%\%~2
exit /b 0

rem :build_test_dir <dir> <tag> <component name or "">
:build_test_dir
set "_TD=%~1"
set "_TT=%~2"
set "_TC=%~3"
if not exist "%_TD%\*.c" exit /b 0
if "%_TC%"=="" goto bt_go
call :skipped %_TC%
if "%SKIPPED%"=="1" ( echo --- SKIPPED by E1_SKIP: tests of %_TC%& exit /b 0 )
:bt_go
set C_EXTRA=/I"%_TD%" /I"%ROOT%\tools" /I"%ROOT%\src\apo" /I"%ROOT%\src\panel"
call :compile "test_%_TT%" "%_TD%"
set "_E=%errorlevel%"
set "C_EXTRA="
if not "%_E%"=="0" exit /b 1
set "_TO=%OBJ%\test_%_TT%"
set "_TCLIB="
if not "%_TC%"=="" if exist "%OBJ%\%_TC%_t.lib" set _TCLIB="%OBJ%\%_TC%_t.lib"
set "_HLIB="
set "_ANY=0"
for %%F in ("%_TD%\test_*.c") do set "_ANY=1"
if "%_ANY%"=="0" goto bt_single
rem helpers -> one library; tools (non-test with an entry point) -> own programs, not run
set "_HRSP=%_TO%\helpers.rsp"
type nul > "%_HRSP%"
set "_TOOLS="
for %%F in ("%_TD%\*.c") do call :bt_classify "%%~fF" "%%~nF"
for %%A in ("%_HRSP%") do set "_HSIZE=%%~zA"
if "%_HSIZE%"=="0" goto bt_progs
lib /nologo /OUT:"%_TO%\helpers.lib" @"%_HRSP%" >nul
if errorlevel 1 ( echo *** lib failed: helpers of %_TT% & exit /b 1 )
set _HLIB="%_TO%\helpers.lib"
:bt_progs
for %%F in ("%_TD%\test_*.c") do call :link_prog "%_TO%\%%~nF.obj" "%%~nF" 1 || exit /b 1
for %%T in (%_TOOLS%) do call :link_prog "%_TO%\%%~T.obj" "%%~T" 0 || exit /b 1
exit /b 0
:bt_single
rem one program from every object of the directory
set "_ARSP=%_TO%\all.rsp"
type nul > "%_ARSP%"
for %%F in ("%_TO%\*.obj") do echo "%%~fF">> "%_ARSP%"
call :link_prog ALLRSP "test_%_TT%" 1 || exit /b 1
exit /b 0

:bt_classify
set "_BN=%~2"
if /i "%_BN:~0,5%"=="test_" exit /b 0
call :has_entry "%~1"
if "%ENTRY%"=="1" ( set "_TOOLS=%_TOOLS% %~2" & exit /b 0 )
echo "%_TO%\%~2.obj">> "%_HRSP%"
exit /b 0

rem :link_prog <main obj or ALLRSP> <exe base name> <run 1/0>
:link_prog
if "%~1"=="ALLRSP" ( set _MAINARG=@"%_ARSP%" ) else ( set _MAINARG="%~1" )
findstr /i /x /c:"%~2" "%OBJ%\testnames.lst" >nul 2>nul && goto lp_dup
echo %~2>> "%OBJ%\testnames.lst"
link %LFLAGS% /SUBSYSTEM:CONSOLE /OUT:"%OUT%\%~2.exe" /PDB:"%OUT%\%~2.pdb" %_MAINARG% %_HLIB% %_TCLIB% %DSPLIB% %COMMONLIB% %APOLIBS% %PANELLIBS% >"%OBJ%\%~2.link.log"
if errorlevel 1 ( type "%OBJ%\%~2.link.log" & echo *** link failed: %~2.exe & exit /b 1 )
if "%~3"=="1" >> "%TESTLIST%" echo %OUT%\%~2.exe^|%_TD%
set /a NBUILT+=1
if "%~3"=="1" echo     -^> %OUT%\%~2.exe
if not "%~3"=="1" echo     -^> %OUT%\%~2.exe  (tool, not run)
exit /b 0
:lp_dup
echo *** duplicate program name %~2.exe - rename one of the test sources
exit /b 1

rem ===========================================================================================
:usage
echo usage: build.cmd [release^|debug] [test] [full] [clean] [nowx] [nops]
exit /b 0
:usage_fail
echo usage: build.cmd [release^|debug] [test] [full] [clean] [nowx] [nops]
exit /b 1
:fail
echo.
echo === BUILD FAILED
exit /b 1
