@echo off
rem ================================================================
rem  DesktopBanner build script (MSVC x64)
rem    build.bat         : build DLL + demo exe into .\bin
rem    build.bat clean   : remove .\bin
rem  Requires: VS2022 (or 2019/2017) with "Desktop development with C++"
rem ================================================================
setlocal
set "ROOT=%~dp0"
set "SRC=%ROOT%src"
set "BIN=%ROOT%bin"

if /i "%~1"=="clean" goto :clean

where cl >nul 2>&1
if not errorlevel 1 goto :havecl

rem ---- locate MSVC via vswhere ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :novs
if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" goto :novs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :novs

:havecl
if not exist "%BIN%" mkdir "%BIN%"
pushd "%SRC%"

echo [1/3] resource
rc /nologo /c 65001 /fo "demo.res" "demo.rc"
if errorlevel 1 goto :err

echo [2/3] DesktopBanner.dll
cl /nologo /LD /MT /O2 /W4 /utf-8 /Zi /Fd:"%BIN%\DesktopBanner.pdb" /DUNICODE /D_UNICODE /Fe:"%BIN%\DesktopBanner.dll" "DesktopBanner.cpp" /link /DEF:"DesktopBanner.def" /IMPLIB:"%BIN%\DesktopBanner.lib" /PDB:"%BIN%\DesktopBanner.pdb" /DEBUG
if errorlevel 1 goto :err

echo [3/3] BannerDemo.exe
cl /nologo /MT /O2 /W4 /utf-8 /Zi /Fd:"%BIN%\BannerDemo.pdb" /DUNICODE /D_UNICODE /Fe:"%BIN%\BannerDemo.exe" "BannerDemo.cpp" "demo.res" user32.lib gdi32.lib comctl32.lib comdlg32.lib ole32.lib /link /SUBSYSTEM:WINDOWS /PDB:"%BIN%\BannerDemo.pdb" /DEBUG
if errorlevel 1 goto :err

popd
del /q "%SRC%\*.obj" 2>nul
del /q "%SRC%\demo.res" 2>nul
echo.
echo BUILD OK  ^-^>  %BIN%
dir /b "%BIN%"
exit /b 0

:err
popd
echo.
echo [ERROR] build failed, see messages above.
exit /b 1

:novs
echo.
echo [ERROR] Visual Studio C++ compiler not found.
echo         Install VS2022 with "Desktop development with C++", or run this script
echo         from a "Developer Command Prompt for VS".
exit /b 1

:clean
if exist "%BIN%" rd /s /q "%BIN%"
echo [clean] %BIN%
exit /b 0
