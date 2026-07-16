@echo off
chcp 65001 >nul 2>&1
setlocal EnableExtensions EnableDelayedExpansion

set "SOURCE_DIR=%~dp0tests"
set "BUILD_DIR=E:\demo\fenxi\lengjing\host_tests"
set "TARGET="

if not exist "%SOURCE_DIR%\CMakeLists.txt" (
    echo [ERROR] Host test project not found: %SOURCE_DIR%
    exit /b 1
)

where cmake.exe >nul 2>&1
if errorlevel 1 (
    echo [ERROR] cmake.exe not found in PATH.
    exit /b 1
)
where ninja.exe >nul 2>&1
if errorlevel 1 (
    echo [ERROR] ninja.exe not found in PATH.
    exit /b 1
)

for /f "tokens=2 delims=( " %%T in ('findstr /R /I /C:"^[ ]*add_executable(" "%SOURCE_DIR%\CMakeLists.txt"') do (
    if not defined TARGET set "TARGET=%%T"
)
if not defined TARGET (
    echo [ERROR] Unable to resolve the host test target.
    exit /b 1
)
set "TARGET=!TARGET:"=!"

if /i "%~1"=="clean" (
    for %%D in ("%BUILD_DIR%") do set "RESOLVED_BUILD=%%~fD"
    for %%D in ("E:\demo\fenxi\lengjing\host_tests") do set "EXPECTED_BUILD=%%~fD"
    if /i not "!RESOLVED_BUILD!"=="!EXPECTED_BUILD!" (
        echo [ERROR] Refusing to clean an unexpected directory: !RESOLVED_BUILD!
        exit /b 1
    )
    if exist "!RESOLVED_BUILD!" rmdir /s /q "!RESOLVED_BUILD!"
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if errorlevel 1 (
    echo [ERROR] Unable to create test build directory: %BUILD_DIR%
    exit /b 1
)

cmake -Wno-deprecated --no-warn-unused-cli -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo [ERROR] Host test configuration failed.
    exit /b 1
)

cmake --build "%BUILD_DIR%" --target "!TARGET!" --parallel 8
if errorlevel 1 (
    echo [ERROR] Host test build failed.
    exit /b 1
)

set "TEST_BIN=%BUILD_DIR%\!TARGET!.exe"
if not exist "!TEST_BIN!" set "TEST_BIN=%BUILD_DIR%\!TARGET!"
if not exist "!TEST_BIN!" (
    echo [ERROR] Host test executable not found for target: !TARGET!
    exit /b 1
)

"!TEST_BIN!"
set "TEST_RESULT=!errorlevel!"
if not "!TEST_RESULT!"=="0" (
    echo [ERROR] Host tests failed with exit code !TEST_RESULT!.
    exit /b !TEST_RESULT!
)

echo [DONE] All host tests passed.
exit /b 0
