@echo off
setlocal EnableExtensions

rem Convert Gatesea QSPI openflashloader binaries into C include arrays.
rem Usage:
rem   update_gatesea_qspi_loader.bat
rem   update_gatesea_qspi_loader.bat build
rem
rem The "build" argument first builds rv32 and rv64 loaders in OPENFLASHLOADER_DIR.

if not defined MSYS2_ROOT set "MSYS2_ROOT=D:\MSYS2"
if not defined OPENFLASHLOADER_DIR set "OPENFLASHLOADER_DIR=F:\VsCode\openflashloader"
if not defined BUILD_LOADER set "BUILD_LOADER=0"

set "ACTION=%~1"
if /I "%ACTION%"=="build" (
	set "BUILD_LOADER=1"
) else if /I "%ACTION%"=="convert" (
	set "BUILD_LOADER=0"
) else if not "%ACTION%"=="" (
	echo ERROR: Unknown argument "%ACTION%".
	echo Usage: %~nx0 [build^|convert]
	exit /b 1
)

for %%I in ("%~dp0.") do set "REPO_ROOT=%%~fI"
set "MSYS2_BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
set "BIN2CHAR=%REPO_ROOT%\src\helper\bin2char.sh"
set "OUT_DIR=%REPO_ROOT%\contrib\loaders\flash\gatesea_qspi"
set "RV32_BIN=%OPENFLASHLOADER_DIR%\build\rv32\loader.bin"
set "RV64_BIN=%OPENFLASHLOADER_DIR%\build\rv64\loader.bin"
set "HOME=%REPO_ROOT%\.tmp"
set "CHERE_INVOKING=1"

if not exist "%MSYS2_BASH%" (
	echo ERROR: MSYS2 bash not found: "%MSYS2_BASH%"
	echo Set MSYS2_ROOT to your MSYS2 install directory.
	exit /b 1
)

if not exist "%BIN2CHAR%" (
	echo ERROR: bin2char.sh not found: "%BIN2CHAR%"
	exit /b 1
)

if "%BUILD_LOADER%"=="1" (
	if not exist "%OPENFLASHLOADER_DIR%\build_gatesea_loader.bat" (
		echo ERROR: build_gatesea_loader.bat not found in "%OPENFLASHLOADER_DIR%".
		echo Set OPENFLASHLOADER_DIR to the openflashloader repository.
		exit /b 1
	)
	echo Building Gatesea QSPI loaders in "%OPENFLASHLOADER_DIR%"...
	pushd "%OPENFLASHLOADER_DIR%" || exit /b 1
	call ".\build_gatesea_loader.bat" rv32
	if errorlevel 1 (
		popd
		exit /b 1
	)
	call ".\build_gatesea_loader.bat" rv64
	if errorlevel 1 (
		popd
		exit /b 1
	)
	popd
)

if not exist "%RV32_BIN%" (
	echo ERROR: rv32 loader not found: "%RV32_BIN%"
	echo Run %~nx0 build, or build it in openflashloader first.
	exit /b 1
)

if not exist "%RV64_BIN%" (
	echo ERROR: rv64 loader not found: "%RV64_BIN%"
	echo Run %~nx0 build, or build it in openflashloader first.
	exit /b 1
)

echo MSYS2_ROOT:          %MSYS2_ROOT%
echo OPENFLASHLOADER_DIR: %OPENFLASHLOADER_DIR%
echo OUT_DIR:             %OUT_DIR%
echo.

"%MSYS2_BASH%" -lc "set -e; repo=$(cygpath -u '%REPO_ROOT%'); ofl=$(cygpath -u '%OPENFLASHLOADER_DIR%'); out=\"$repo/contrib/loaders/flash/gatesea_qspi\"; mkdir -p \"$out\"; \"$repo/src/helper/bin2char.sh\" < \"$ofl/build/rv32/loader.bin\" > \"$out/riscv32_gatesea_qspi.inc\"; \"$repo/src/helper/bin2char.sh\" < \"$ofl/build/rv64/loader.bin\" > \"$out/riscv64_gatesea_qspi.inc\""
if errorlevel 1 (
	echo.
	echo ERROR: Failed to update Gatesea QSPI loader include files.
	exit /b 1
)

echo Updated:
echo   %OUT_DIR%\riscv32_gatesea_qspi.inc
echo   %OUT_DIR%\riscv64_gatesea_qspi.inc
echo.
echo Rebuild OpenOCD after this: build_mingw64_liteos.bat

endlocal
