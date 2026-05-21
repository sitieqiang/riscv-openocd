@echo off
setlocal EnableExtensions

rem Build OpenOCD with MSYS2 MinGW64.
rem Defaults match the LiteOS verification build used on this workspace.

if not defined MSYS2_ROOT set "MSYS2_ROOT=D:\MSYS2"
if not defined BUILD_DIR set "BUILD_DIR=build-mingw64-liteos"
if not defined JOBS set "JOBS=%NUMBER_OF_PROCESSORS%"
if not defined JOBS set "JOBS=2"
if not defined GENERATE_COMPILE_COMMANDS set "GENERATE_COMPILE_COMMANDS=1"
if not defined COPY_COMPILE_COMMANDS_TO_ROOT set "COPY_COMPILE_COMMANDS_TO_ROOT=1"
if not defined CLEAN_BUILD set "CLEAN_BUILD=0"

set "MSYS2_BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
set "CONFIGURE_FLAGS=--enable-jlink --enable-internal-libjaylink --enable-internal-jimtcl --disable-werror --disable-buspirate --disable-ftd2xx"
set "ACTION=build"
if /I "%~1"=="clean" (
	set "ACTION=clean"
	shift /1
)
set "EXTRA_CONFIGURE_FLAGS=%*"

if /I not "%ACTION%"=="clean" (
	if not exist "%MSYS2_BASH%" (
		echo ERROR: MSYS2 bash not found: "%MSYS2_BASH%"
		echo Set MSYS2_ROOT to your MSYS2 install directory and run again.
		exit /b 1
	)
)

pushd "%~dp0" || exit /b 1

if /I "%ACTION%"=="clean" goto clean_action

echo MSYS2_ROOT: %MSYS2_ROOT%
echo BUILD_DIR:  %BUILD_DIR%
echo JOBS:       %JOBS%
echo COMPILE_COMMANDS: %GENERATE_COMPILE_COMMANDS%
echo CLEAN_BUILD: %CLEAN_BUILD%
echo.

set "HOME=%CD%\.tmp"
set "CHERE_INVOKING=1"

"%MSYS2_BASH%" -lc "set -e; export PATH=/mingw64/bin:/usr/bin:$PATH; mkdir -p .tmp \"$BUILD_DIR\"; tmpdir=$(pwd)/.tmp; export TMPDIR=$tmpdir TEMP=$tmpdir TMP=$tmpdir; need_bootstrap=0; for f in configure build-aux/config.guess build-aux/config.sub build-aux/ltmain.sh build-aux/compile build-aux/missing build-aux/install-sh; do if [ ! -f \"$f\" ]; then need_bootstrap=1; fi; done; if [ \"$need_bootstrap\" != \"0\" ]; then ./bootstrap nosubmodule; fi; cd \"$BUILD_DIR\"; ../configure $CONFIGURE_FLAGS $EXTRA_CONFIGURE_FLAGS; if [ \"$CLEAN_BUILD\" != \"0\" ]; then make clean; fi; if [ \"$GENERATE_COMPILE_COMMANDS\" != \"0\" ] && command -v compiledb >/dev/null 2>&1; then set +e; make -j\"$JOBS\" V=1 2>&1 | tee compile_commands.build.log; make_status=${PIPESTATUS[0]}; set -e; echo Generating compile_commands.json; if compiledb -p compile_commands.build.log -f -o compile_commands.json; then if [ \"$COPY_COMPILE_COMMANDS_TO_ROOT\" != \"0\" ]; then cp compile_commands.json ../compile_commands.json || echo WARNING: failed to copy compile_commands.json to repository root.; fi; else echo WARNING: failed to generate compile_commands.json.; fi; exit $make_status; else if [ \"$GENERATE_COMPILE_COMMANDS\" != \"0\" ]; then echo WARNING: compiledb not found in MSYS2. Install mingw-w64-x86_64-compiledb or set GENERATE_COMPILE_COMMANDS=0.; fi; make -j\"$JOBS\"; fi"
set "BUILD_RESULT=%ERRORLEVEL%"

if "%BUILD_RESULT%"=="0" (
	echo.
	echo Build succeeded: %CD%\%BUILD_DIR%\src\openocd.exe
	if "%GENERATE_COMPILE_COMMANDS%"=="0" (
		rem compile_commands.json generation disabled.
	) else if "%COPY_COMPILE_COMMANDS_TO_ROOT%"=="0" (
		echo Compilation database: %CD%\%BUILD_DIR%\compile_commands.json
	) else (
		echo Compilation database: %CD%\compile_commands.json
	)
) else (
	echo.
	echo Build failed with exit code %BUILD_RESULT%.
)

popd
exit /b %BUILD_RESULT%

:clean_action
call :clean_build_files
set "BUILD_RESULT=%ERRORLEVEL%"
popd
exit /b %BUILD_RESULT%

:clean_build_files
setlocal EnableDelayedExpansion
set "REPO_ROOT=%CD%"

if "%BUILD_DIR%"=="" (
	echo ERROR: BUILD_DIR is empty.
	endlocal & exit /b 1
)

for %%I in ("%BUILD_DIR%") do set "BUILD_DIR_FULL=%%~fI"

set "REPO_PREFIX=!REPO_ROOT!\"
call set "BUILD_DIR_REL=%%BUILD_DIR_FULL:%REPO_PREFIX%=%%"
if /I "!BUILD_DIR_REL!"=="!BUILD_DIR_FULL!" (
	echo ERROR: Refusing to clean BUILD_DIR outside repository: !BUILD_DIR_FULL!
	endlocal & exit /b 1
)
if "!BUILD_DIR_REL!"=="" (
	echo ERROR: Refusing to clean repository root as BUILD_DIR.
	endlocal & exit /b 1
)

echo Cleaning build files...
echo BUILD_DIR: !BUILD_DIR_FULL!

if exist "!BUILD_DIR_FULL!\" (
	echo Removing directory: !BUILD_DIR_FULL!
	rmdir /s /q "!BUILD_DIR_FULL!"
	if exist "!BUILD_DIR_FULL!\" (
		echo ERROR: Failed to remove directory: !BUILD_DIR_FULL!
		endlocal & exit /b 1
	)
) else (
	echo Build directory not found: !BUILD_DIR_FULL!
)

if exist "!REPO_ROOT!\compile_commands.json" (
	echo Removing file: !REPO_ROOT!\compile_commands.json
	del /f /q "!REPO_ROOT!\compile_commands.json"
	if exist "!REPO_ROOT!\compile_commands.json" (
		echo ERROR: Failed to remove file: !REPO_ROOT!\compile_commands.json
		endlocal & exit /b 1
	)
)

if exist "!REPO_ROOT!\.tmp\" (
	echo Removing directory: !REPO_ROOT!\.tmp
	rmdir /s /q "!REPO_ROOT!\.tmp"
	if exist "!REPO_ROOT!\.tmp\" (
		echo ERROR: Failed to remove directory: !REPO_ROOT!\.tmp
		endlocal & exit /b 1
	)
)

echo Clean complete.
endlocal & exit /b 0
