@echo off
setlocal EnableExtensions

rem Build OpenOCD with MSYS2 MinGW64.
rem Defaults match the LiteOS verification build used on this workspace.

if not defined MSYS2_ROOT set "MSYS2_ROOT=D:\MSYS2"
if not defined BUILD_DIR set "BUILD_DIR=build-mingw64-liteos"
if not defined JOBS set "JOBS=%NUMBER_OF_PROCESSORS%"
if not defined JOBS set "JOBS=2"

set "MSYS2_BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
set "CONFIGURE_FLAGS=--enable-jlink --enable-internal-libjaylink --enable-internal-jimtcl --disable-werror --disable-buspirate --disable-ftd2xx"
set "EXTRA_CONFIGURE_FLAGS=%*"

if not exist "%MSYS2_BASH%" (
	echo ERROR: MSYS2 bash not found: "%MSYS2_BASH%"
	echo Set MSYS2_ROOT to your MSYS2 install directory and run again.
	exit /b 1
)

pushd "%~dp0" || exit /b 1

echo MSYS2_ROOT: %MSYS2_ROOT%
echo BUILD_DIR:  %BUILD_DIR%
echo JOBS:       %JOBS%
echo.

set "HOME=%CD%\.tmp"
set "CHERE_INVOKING=1"

"%MSYS2_BASH%" -lc "set -e; export PATH=/mingw64/bin:/usr/bin:$PATH; mkdir -p .tmp %BUILD_DIR%; tmpdir=$(pwd)/.tmp; export TMPDIR=$tmpdir TEMP=$tmpdir TMP=$tmpdir; if [ ! -f configure ]; then ./bootstrap nosubmodule; fi; cd %BUILD_DIR%; ../configure %CONFIGURE_FLAGS% %EXTRA_CONFIGURE_FLAGS%; make -j%JOBS%"
set "BUILD_RESULT=%ERRORLEVEL%"

if "%BUILD_RESULT%"=="0" (
	echo.
	echo Build succeeded: %CD%\%BUILD_DIR%\src\openocd.exe
) else (
	echo.
	echo Build failed with exit code %BUILD_RESULT%.
)

popd
exit /b %BUILD_RESULT%
