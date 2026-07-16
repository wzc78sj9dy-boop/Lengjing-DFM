@echo off
chcp 65001 >nul 2>&1
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
set "SOURCE_DIR=%ROOT%jni"
set "BUILD_DIR=%SOURCE_DIR%\build"
set "NDK=D:\AAA\android-ndk-r27d"
set "TARGET="
set "PRODUCT="

if not exist "%NDK%\build\cmake\android.toolchain.cmake" (
    echo [ERROR] Required customized NDK not found: %NDK%
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
for /f "tokens=2" %%P in ('findstr /R /I /C:"^[ ]*OUTPUT_NAME[ ]" "%SOURCE_DIR%\CMakeLists.txt"') do (
    if not defined PRODUCT set "PRODUCT=%%P"
)
if not defined TARGET (
    echo [ERROR] Unable to resolve the Android build target.
    exit /b 1
)
set "TARGET=!TARGET:"=!"
if not defined PRODUCT set "PRODUCT=!TARGET!"
set "PRODUCT=!PRODUCT:"=!"

if /i "%~1"=="clean" (
    for %%D in ("%BUILD_DIR%") do set "RESOLVED_BUILD=%%~fD"
    for %%D in ("%ROOT%jni\build") do set "EXPECTED_BUILD=%%~fD"
    if /i not "!RESOLVED_BUILD!"=="!EXPECTED_BUILD!" (
        echo [ERROR] Refusing to clean an unexpected directory: !RESOLVED_BUILD!
        exit /b 1
    )
    if exist "!RESOLVED_BUILD!" rmdir /s /q "!RESOLVED_BUILD!"
)

set "NDK_CMAKE=%NDK:\=/%"
echo [NDK] %NDK%
echo [TARGET] !TARGET!

cmake -Wno-deprecated --no-warn-unused-cli -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE="%NDK_CMAKE%/build/cmake/android.toolchain.cmake" ^
    -DANDROID_ABI=arm64-v8a ^
    -DANDROID_PLATFORM=android-21 ^
    -DANDROID_STL=c++_static ^
    -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo [ERROR] Android configuration failed.
    exit /b 1
)

cmake --build "%BUILD_DIR%" --target "!TARGET!" --parallel 8
if errorlevel 1 (
    echo [ERROR] Android build failed.
    exit /b 1
)

set "BIN=%BUILD_DIR%\!PRODUCT!"
if not exist "!BIN!" (
    for /f "tokens=2" %%F in ('findstr /R /C:"^build .*: CXX_EXECUTABLE_LINKER" "%BUILD_DIR%\build.ninja"') do (
        if not defined LINK_OUTPUT set "LINK_OUTPUT=%%F"
    )
    if defined LINK_OUTPUT (
        set "LINK_OUTPUT=!LINK_OUTPUT:~0,-1!"
        set "LINK_OUTPUT=!LINK_OUTPUT:/=\!"
        set "BIN=%BUILD_DIR%\!LINK_OUTPUT!"
    )
)
if not exist "!BIN!" (
    echo [ERROR] Product not found for target: !TARGET!
    exit /b 1
)

for %%I in ("!BIN!") do echo [DONE] %%~fI ^(%%~zI bytes^)
exit /b 0
