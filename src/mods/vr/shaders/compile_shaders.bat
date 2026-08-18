@echo off
rem Some code is from DirectXTK's CompileShaders.cmd

setlocal
set error=0

if %PROCESSOR_ARCHITECTURE%.==ARM64. (set FXCARCH=arm64) else (if %PROCESSOR_ARCHITECTURE%.==AMD64. (set FXCARCH=x64) else (set FXCARCH=x86))

set FXCOPTS=/nologo /WX /Ges /Zi /Zpc /Qstrip_reflect /Qstrip_debug

set PCFXC="%WindowsSdkVerBinPath%%FXCARCH%\fxc.exe"
if exist %PCFXC% goto continue
set PCFXC="%WindowsSdkBinPath%%WindowsSDKVersion%\%FXCARCH%\fxc.exe"
if exist %PCFXC% goto continue
set PCFXC="%WindowsSdkDir%bin\%WindowsSDKVersion%\%FXCARCH%\fxc.exe"
if exist %PCFXC% goto continue

set PCFXC=fxc.exe

:continue

if not defined CompileShadersOutput set CompileShadersOutput=Compiled
set StrTrim=%CompileShadersOutput%##
set StrTrim=%StrTrim: ##=%
set CompileShadersOutput=%StrTrim:##=%
@if not exist "%CompileShadersOutput%" mkdir "%CompileShadersOutput%"

rem The compiling part.
call :CompileShader alpha_luminance_sprite_ps ps SpritePixelShader
call :CompileShader alpha_luminance_sprite_ps vs SpriteVertexShader

if %error% == 0 (
    echo Shaders compiled ok
) else (
    echo There were shader compilation errors!
    exit /b 1
)

endlocal
exit /b 0

:CompileShader
set SOURCEFILE=%1.fx
if not exist "%SOURCEFILE%" set SOURCEFILE=%1.hlsl
if not exist "%SOURCEFILE%" (
    echo Missing shader source: %1.fx or %1.hlsl
    set error=1
    exit /b
)

set attempt_error=0
REM Compile into a temp header, then only replace the real .inc when the bytes
REM actually changed (see :FinalizeInc). DXC/FXC output is deterministic with
REM -Qstrip_debug/-Qstrip_reflect, so an unchanged shader keeps its old
REM timestamp and MSBuild skips recompiling every TU that includes it.
set OUTINC=%CompileShadersOutput%\%1_%3.inc
set TMPINC=%CompileShadersOutput%\%1_%3.inc.tmp

if defined PCDXC (
    set DXCOPTS=-nologo -WX -Ges -Zi -Zpc -Qstrip_reflect -Qstrip_debug -HV 2021
    set CMD=%PCDXC% "!SOURCEFILE!" !DXCOPTS! -T %2_6_0 -E %3 -Fh"!TMPINC!" -Fd"%CompileShadersOutput%\%1_%3.pdb" -Vn%1_%3
    echo.
    echo !CMD!
    !CMD! || set attempt_error=1
    if !attempt_error! == 0 (
        call :FinalizeInc "!TMPINC!" "!OUTINC!"
        exit /b
    )
    echo DXC compile failed for %SOURCEFILE%, trying FXC fallback...
)

if not defined PCFXC (
    set error=1
    exit /b
)

set FXCOPTS=/nologo /WX /Ges /Zi /Zpc /Qstrip_reflect /Qstrip_debug
set CMD=%PCFXC% "!SOURCEFILE!" !FXCOPTS! /T%2_5_0 /E%3 "/Fh!TMPINC!" "/Fd%CompileShadersOutput%\%1_%3.pdb" /Vn%1_%3
echo.
echo !CMD!
!CMD! || set error=1
if !error! == 0 call :FinalizeInc "!TMPINC!" "!OUTINC!"
exit /b

:FinalizeInc
REM %~1 = freshly compiled temp header, %~2 = destination header. Keep the
REM destination untouched (stable timestamp) when the compiled bytes match.
if not exist "%~1" exit /b
if not exist "%~2" (
    move /y "%~1" "%~2" >nul
    exit /b
)
fc /b "%~1" "%~2" >nul 2>&1
if !errorlevel! == 0 (
    del "%~1" >nul 2>&1
) else (
    move /y "%~1" "%~2" >nul
)
exit /b
4
:CompileShaderHLSL
set fxc=%PCFXC% "%1.hlsl" %FXCOPTS% /T%2_5_0 /E%3 "/Fh%CompileShadersOutput%\%1_%3.inc" "/Fd%CompileShadersOutput%\%1_%3.pdb" /Vn%1_%3
echo.
echo %fxc%
%fxc% || set error=1
exit /b