@echo off
rem ---------------------------------------------------------------------------------------------
rem build.cmd - builds e1panel.exe (src\panel) and the panel unit tests, then runs the tests.
rem
rem   tests\panel\build.cmd           build + run tests
rem   tests\panel\build.cmd notest    build only
rem
rem Output: tests\panel\build\e1panel.exe, test_panel.exe, mkicon.exe, e1panel.ico
rem Toolchain: MSVC 2022 x64 + Windows SDK (vcvars64 is called when not already set up).
rem
rem Dependencies taken from the tree: src\dsp\*.c (libe1dsp, for e1fx_params_* and the EQ /
rem VoiceFX preset tables) and src\common\e1fx_ipc.c. They are archived into a .lib so the
rem linker only pulls the objects the panel really uses.
rem Optional: set E1_STUBS=<dir> holding stub_*.c for components that are not available yet;
rem the stub library is linked AFTER the real one, so real code always wins.
rem ---------------------------------------------------------------------------------------------
setlocal EnableDelayedExpansion
set "HERE=%~dp0"
for %%I in ("%HERE%..\..") do set "ROOT=%%~fI"
set "SRC=%ROOT%\src\panel"
set "OUT=%HERE%build"
set "OBJ=%OUT%\obj"

if not defined VCToolsInstallDir (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
    if errorlevel 1 ( echo [build] vcvars64.bat failed & exit /b 1 )
)
for %%D in ("%OUT%" "%OBJ%" "%OBJ%\panel" "%OBJ%\deps" "%OBJ%\stubs" "%OBJ%\test") do if not exist "%%~D" mkdir "%%~D"

set "INC=/I"%ROOT%\include" /I"%ROOT%\src\dsp" /I"%SRC%""
set "CF=/nologo /c /W4 /O2 /std:c11 /utf-8 /fp:precise /MT /GS /guard:cf /Zi /DUNICODE /D_UNICODE"
set "SYSLIBS=user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib comdlg32.lib shlwapi.lib dwmapi.lib gdiplus.lib msimg32.lib uuid.lib"

rem ---- panel sources (warnings are errors) --------------------------------------------------------
echo [build] compiling src\panel
pushd "%OBJ%\panel"
del /q *.obj 2>nul
cl %CF% /WX %INC% /Fd"%OBJ%\panel\vc.pdb" "%SRC%\*.c"
if errorlevel 1 ( popd & echo [build] FAILED: panel sources & exit /b 1 )
popd

rem ---- dependencies from the tree ----------------------------------------------------------------
echo [build] compiling dependencies (src\dsp, src\common)
pushd "%OBJ%\deps"
del /q *.obj *.lib 2>nul
set "DEPFAIL="
for %%F in ("%ROOT%\src\dsp\*.c" "%ROOT%\src\common\e1fx_ipc.c") do (
    if exist "%%~F" (
        cl %CF% %INC% /Fd"%OBJ%\deps\vc.pdb" "%%~F" >"%%~nF.log" 2>&1
        if errorlevel 1 ( echo [build]   skipped %%~nxF ^(does not compile yet, see obj\deps\%%~nF.log^) & set "DEPFAIL=1" )
    )
)
set "DEPLIB="
if exist *.obj ( lib /nologo /OUT:e1deps.lib *.obj >nul && set "DEPLIB="%OBJ%\deps\e1deps.lib"" )
popd

set "STUBLIB="
if defined E1_STUBS (
    echo [build] compiling stubs from %E1_STUBS%
    pushd "%OBJ%\stubs"
    del /q *.obj *.lib 2>nul
    cl %CF% %INC% /Fd"%OBJ%\stubs\vc.pdb" "%E1_STUBS%\*.c" >stubs.log 2>&1
    if errorlevel 1 ( type stubs.log & popd & echo [build] FAILED: stubs & exit /b 1 )
    lib /nologo /OUT:e1stubs.lib *.obj >nul
    set "STUBLIB="%OBJ%\stubs\e1stubs.lib""
    popd
)

rem ---- icon --------------------------------------------------------------------------------------
pushd "%OUT%"
cl %CF% /WX %INC% /Fo"%OBJ%\test\mkicon.obj" /Fd"%OBJ%\test\vc.pdb" "%HERE%mkicon.c" >nul
if errorlevel 1 ( popd & echo [build] FAILED: mkicon & exit /b 1 )
link /nologo /SUBSYSTEM:CONSOLE /OUT:mkicon.exe "%OBJ%\test\mkicon.obj" "%OBJ%\panel\gfx.obj" %SYSLIBS% >nul
if errorlevel 1 ( popd & echo [build] FAILED: link mkicon & exit /b 1 )
"%OUT%\mkicon.exe" "%OUT%\e1panel.ico"
if errorlevel 1 ( popd & echo [build] FAILED: icon & exit /b 1 )
rc /nologo /I"%SRC%" /I"%OUT%" /fo "%OBJ%\e1panel.res" "%SRC%\e1panel.rc"
if errorlevel 1 ( popd & echo [build] FAILED: resources & exit /b 1 )

rem ---- link e1panel.exe --------------------------------------------------------------------------
set "LF=/nologo /SUBSYSTEM:WINDOWS /DEBUG /OPT:REF /OPT:ICF /guard:cf /DYNAMICBASE /NXCOMPAT /MANIFEST:NO"
echo [build] linking e1panel.exe
link %LF% /OUT:e1panel.exe /PDB:e1panel.pdb "%OBJ%\panel\*.obj" "%OBJ%\e1panel.res" %DEPLIB% %STUBLIB% %SYSLIBS% >link.log 2>&1
if errorlevel 1 (
    if defined STUBLIB (
        echo [build]   link against the tree failed ^(see build\link.log^); retrying with stubs only
        link %LF% /OUT:e1panel.exe /PDB:e1panel.pdb "%OBJ%\panel\*.obj" "%OBJ%\e1panel.res" %STUBLIB% %SYSLIBS% >link2.log 2>&1
        if errorlevel 1 ( type link2.log & popd & echo [build] FAILED: link & exit /b 1 )
        set "DEPLIB="
    ) else ( type link.log & popd & echo [build] FAILED: link & exit /b 1 )
)

rem ---- unit tests --------------------------------------------------------------------------------
echo [build] building test_panel.exe
cl %CF% /WX %INC% /Fo"%OBJ%\test\test_panel.obj" /Fd"%OBJ%\test\vc.pdb" "%HERE%test_panel.c" >"%OBJ%\test\cl.log" 2>&1
if errorlevel 1 ( type "%OBJ%\test\cl.log" & popd & echo [build] FAILED: test compile & exit /b 1 )
set "TOBJ="%OBJ%\test\test_panel.obj" "%OBJ%\panel\profile.obj" "%OBJ%\panel\eqpreset.obj" "%OBJ%\panel\eqcurve.obj" "%OBJ%\panel\strings.obj" "%OBJ%\panel\sync.obj""
link /nologo /SUBSYSTEM:CONSOLE /DEBUG /OUT:test_panel.exe %TOBJ% %DEPLIB% %STUBLIB% %SYSLIBS% >linkt.log 2>&1
if errorlevel 1 ( type linkt.log & popd & echo [build] FAILED: test link & exit /b 1 )
echo [build] OK: %OUT%\e1panel.exe
if /i "%~1"=="notest" ( popd & exit /b 0 )
echo [build] running tests
"%OUT%\test_panel.exe"
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
