@echo off
rem build_and_test.cmd  -  builds e1fx.dll + apohost.exe into tests\apo\out and runs the tests.
rem   set E1_DSP_STUB_DIR=<dir> to link a stub DSP chain instead of src\dsp (see src\apo\build_e1fx.cmd)
rem No installation, no registration, no system changes: the host loads the DLL directly, uses a
rem session-local section (Local\E1Test_<pid>) and a settings file under %TEMP%.
setlocal
set "T=%~dp0"
for %%i in ("%T%..\..") do set "ROOT=%%~fi"
set "OUT=%T%out"
for %%i in ("%OUT%") do set "OUT=%%~fi"
if not exist "%OUT%\obj_host" mkdir "%OUT%\obj_host"
if not defined VCToolsInstallDir call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if not defined VCToolsInstallDir (echo [test] vcvars64.bat failed & exit /b 1)

call "%ROOT%\src\apo\build_e1fx.cmd" "%OUT%" || (echo [test] DLL build failed & exit /b 1)

cl /nologo /W4 /WX /O2 /fp:precise /std:c11 /MT /Zi /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 ^
    /I"%ROOT%\include" /I"%ROOT%\src\dsp" /Fo"%OUT%\obj_host\\" /Fd"%OUT%\obj_host\host.pdb" ^
    "%T%apohost.c" "%ROOT%\src\common\e1fx_ipc.c" "%ROOT%\src\apo\e1fx_mediatype.c" "%ROOT%\src\apo\e1fx_iids.c" ^
    /Fe"%OUT%\apohost.exe" /link /DEBUG "%OUT%\e1dsp_sel.lib" ole32.lib advapi32.lib uuid.lib || (echo [test] host build failed & exit /b 1)

"%OUT%\apohost.exe" "%OUT%\e1fx.dll"
exit /b %ERRORLEVEL%
