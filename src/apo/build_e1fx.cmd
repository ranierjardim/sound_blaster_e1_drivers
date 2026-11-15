@echo off
rem build_e1fx.cmd [outdir]  -  builds e1fx.dll (x64) and e1dsp_sel.lib (the DSP sources it was linked with)
rem
rem DSP sources:
rem   default                 every src\dsp\*.c (libe1dsp) plus src\common\e1fx_params.c if it exists
rem   E1_DSP_STUB_DIR=<dir>   every <dir>\*.c instead (test stub; never inside the project tree)
rem   E1_DSP_EXTRA_DIR=<dir>  default sources plus <dir>\*.c (stand-ins for modules not written yet)
rem Output: <outdir>\e1fx.dll, e1fx.pdb, e1dsp_sel.lib ; default outdir = src\apo\out
setlocal EnableDelayedExpansion
set "APO=%~dp0"
for %%i in ("%APO%..\..") do set "ROOT=%%~fi"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%APO%out"
for %%i in ("%OUT%") do set "OUT=%%~fi"
set "OBJ=%OUT%\obj_e1fx"
if not exist "%OBJ%" mkdir "%OBJ%"
if exist "%OBJ%\dsp" rmdir /s /q "%OBJ%\dsp"
mkdir "%OBJ%\dsp"

if not defined VCToolsInstallDir call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if not defined VCToolsInstallDir (echo [e1fx] vcvars64.bat failed & exit /b 1)

set "DSPSRC="
if defined E1_DSP_STUB_DIR (
    for %%f in ("%E1_DSP_STUB_DIR%\*.c") do set DSPSRC=!DSPSRC! "%%~ff"
    set "DSPKIND=stub from %E1_DSP_STUB_DIR%"
) else (
    for %%f in ("%ROOT%\src\dsp\*.c") do set DSPSRC=!DSPSRC! "%%~ff"
    if exist "%ROOT%\src\common\e1fx_params.c" set DSPSRC=!DSPSRC! "%ROOT%\src\common\e1fx_params.c"
    set "DSPKIND=libe1dsp from src\dsp"
    if defined E1_DSP_EXTRA_DIR (
        for %%f in ("%E1_DSP_EXTRA_DIR%\*.c") do set DSPSRC=!DSPSRC! "%%~ff"
        set "DSPKIND=libe1dsp from src\dsp + extra sources from %E1_DSP_EXTRA_DIR%"
    )
)
if "!DSPSRC!"=="" (echo [e1fx] no DSP sources found ^(src\dsp\*.c empty; set E1_DSP_STUB_DIR for a stub^) & exit /b 1)
echo [e1fx] DSP: !DSPKIND!

set "COMMON=/nologo /c /W4 /O2 /fp:precise /std:c11 /MT /GS /guard:cf /Zi /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /I"%ROOT%\include" /I"%ROOT%\src\dsp""

cl %COMMON% /Fo"%OBJ%\dsp\\" /Fd"%OBJ%\dsp.pdb" !DSPSRC! || exit /b 1
lib /nologo /OUT:"%OUT%\e1dsp_sel.lib" "%OBJ%\dsp\*.obj" || exit /b 1

cl %COMMON% /WX /Fo"%OBJ%\\" /Fd"%OBJ%\e1fx_c.pdb" ^
    "%APO%e1fx_apo.c" "%APO%e1fx_dll.c" "%APO%e1fx_mediatype.c" "%APO%e1fx_iids.c" ^
    "%ROOT%\src\common\e1fx_ipc.c" || exit /b 1
rc /nologo /fo "%OBJ%\e1fx.res" "%APO%e1fx.rc" || exit /b 1

link /nologo /DLL /MACHINE:X64 /DEF:"%APO%e1fx.def" /OUT:"%OUT%\e1fx.dll" /PDB:"%OUT%\e1fx.pdb" /DEBUG ^
    /OPT:REF /OPT:ICF /guard:cf /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA ^
    "%OBJ%\e1fx_apo.obj" "%OBJ%\e1fx_dll.obj" "%OBJ%\e1fx_mediatype.obj" "%OBJ%\e1fx_iids.obj" ^
    "%OBJ%\e1fx_ipc.obj" "%OBJ%\e1fx.res" "%OUT%\e1dsp_sel.lib" ^
    ole32.lib advapi32.lib kernel32.lib || exit /b 1
echo [e1fx] built %OUT%\e1fx.dll
exit /b 0
