@echo off
setlocal
rem DirectMotion 3.0.0 : AviSynth+ Prefetch owns frame-level parallelism; MVTools internal AVSTP threading is disabled.
if "%AVISYNTH_ROOT%"=="" (
  echo Set AVISYNTH_ROOT to an AviSynth+ SDK/source root containing avisynth.h.
  exit /b 20
)
if not exist "%AVISYNTH_ROOT%\include\avisynth.h" if not exist "%AVISYNTH_ROOT%\avs_core\include\avisynth.h" (
  echo ERROR: avisynth.h not found below AVISYNTH_ROOT=%AVISYNTH_ROOT%
  exit /b 21
)
where msbuild >nul 2>nul
if errorlevel 1 (
  echo MSBuild not found. Run this script from a Visual Studio Developer Command Prompt.
  exit /b 22
)
if "%MSVC_PLATFORM_TOOLSET%"=="" set "MSVC_PLATFORM_TOOLSET=v143"
set "TOOLSET_ARG=/p:PlatformToolset=%MSVC_PLATFORM_TOOLSET%"
msbuild "%~dp0directmotion.sln" /m /v:minimal /p:Configuration=Release /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0 /p:AviSynthRoot="%AVISYNTH_ROOT%" %TOOLSET_ARG%
set "BUILD_EXIT_CODE=%ERRORLEVEL%"
if not "%BUILD_EXIT_CODE%"=="0" exit /b %BUILD_EXIT_CODE%
if not exist "%~dp0bin_x64\lib\directmotion.ax" (
  echo ERROR: build reported success but bin_x64\lib\directmotion.ax is missing.
  exit /b 23
)
echo directmotion 3.0.0 AviSynth+ Prefetch build complete.
echo Place matching x64 avisynth.dll and mvtools2.dll beside bin_x64\lib\directmotion.ax before registering the filter. avstp.dll is not required by DirectMotion's MVTools graph: MSuper and MAnalyse explicitly use mt=false, and AviSynth+ Prefetch owns frame-level parallelism.
echo This is a source-build success only; validate playback and seek behavior on the target system.
exit /b 0
