@echo off
chcp 65001 >nul 2>&1
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
set "SOURCE_DIR=%ROOT%jni"
set "BUILD_DIR=%SOURCE_DIR%\build"
set "NDK="
set "NINJA="
set "TARGET="
set "PRODUCT="
set "AUTH_CONFIG="
set "PROJECTILE_TRACKING=OFF"
set "BUILD_RESULT=0"
set "CLEAN_BUILD=0"
set "PAUSE_ON_EXIT=1"

if /i "%LENGJING_NO_PAUSE%"=="1" set "PAUSE_ON_EXIT=0"
if defined CI set "PAUSE_ON_EXIT=0"

:parse_build_args
if "%~1"=="" goto :build_args_done
if /i "%~1"=="clean" (
    set "CLEAN_BUILD=1"
    shift
    goto :parse_build_args
)
if /i "%~1"=="--no-pause" (
    set "PAUSE_ON_EXIT=0"
    shift
    goto :parse_build_args
)
echo [ERROR] Unsupported option: %~1
echo [USAGE] 1_build.bat [clean] [--no-pause]
set "BUILD_RESULT=1"
goto :finish

:build_args_done

if defined LENGJING_AUTH_CONFIG (
    if not exist "%LENGJING_AUTH_CONFIG%" (
        echo [ERROR] LENGJING_AUTH_CONFIG does not exist: %LENGJING_AUTH_CONFIG%
        set "BUILD_RESULT=1"
        goto :finish
    )
    set "AUTH_CONFIG=%LENGJING_AUTH_CONFIG%"
)
if not defined AUTH_CONFIG if exist "%SOURCE_DIR%\cmake\AuthConfigPrivate.cmake" (
    set "AUTH_CONFIG=%SOURCE_DIR%\cmake\AuthConfigPrivate.cmake"
)

if exist "D:\AAA\android-ndk-r27d\build\cmake\android.toolchain.cmake" set "NDK=D:\AAA\android-ndk-r27d"
if not defined NDK if defined ANDROID_NDK_HOME if exist "%ANDROID_NDK_HOME%\build\cmake\android.toolchain.cmake" set "NDK=%ANDROID_NDK_HOME%"
if not defined NDK if defined ANDROID_NDK_ROOT if exist "%ANDROID_NDK_ROOT%\build\cmake\android.toolchain.cmake" set "NDK=%ANDROID_NDK_ROOT%"
if not defined NDK if defined NDK_HOME if exist "%NDK_HOME%\build\cmake\android.toolchain.cmake" set "NDK=%NDK_HOME%"
if not defined NDK if exist "D:\AAA\android-ndk-r28\build\cmake\android.toolchain.cmake" set "NDK=D:\AAA\android-ndk-r28"
if not defined NDK if exist "D:\AAA\android-ndk-r29\build\cmake\android.toolchain.cmake" set "NDK=D:\AAA\android-ndk-r29"
if not defined NDK call :probe_sdk_ndk

if not exist "%NDK%\build\cmake\android.toolchain.cmake" (
    echo [ERROR] Required Android NDK not found.
    set "BUILD_RESULT=1"
    goto :finish
)
where cmake.exe >nul 2>&1
if errorlevel 1 (
    echo [ERROR] cmake.exe not found in PATH.
    set "BUILD_RESULT=1"
    goto :finish
)
if defined NINJA_PATH (
    if exist "%NINJA_PATH%\ninja.exe" set "NINJA=%NINJA_PATH%\ninja.exe"
    if not defined NINJA if exist "%NINJA_PATH%" set "NINJA=%NINJA_PATH%"
)
if not defined NINJA for %%I in (ninja.exe) do set "NINJA=%%~$PATH:I"
if not defined NINJA if exist "E:\demo\fenxi\lengjing\tools\python\bin\ninja.exe" (
    set "NINJA=E:\demo\fenxi\lengjing\tools\python\bin\ninja.exe"
)
if not defined NINJA (
    echo [ERROR] ninja.exe not found in PATH.
    set "BUILD_RESULT=1"
    goto :finish
)

for /f "tokens=2 delims=( " %%T in ('findstr /R /I /C:"^[ ]*add_executable(" "%SOURCE_DIR%\CMakeLists.txt"') do (
    if not defined TARGET set "TARGET=%%T"
)
for /f "tokens=2" %%P in ('findstr /R /I /C:"^[ ]*OUTPUT_NAME[ ]" "%SOURCE_DIR%\CMakeLists.txt"') do (
    if not defined PRODUCT set "PRODUCT=%%P"
)
if not defined TARGET (
    echo [ERROR] Unable to resolve the Android build target.
    set "BUILD_RESULT=1"
    goto :finish
)
set "TARGET=!TARGET:"=!"
if not defined PRODUCT set "PRODUCT=!TARGET!"
set "PRODUCT=!PRODUCT:"=!"

if "!CLEAN_BUILD!"=="1" (
    for %%D in ("%BUILD_DIR%") do set "RESOLVED_BUILD=%%~fD"
    for %%D in ("%ROOT%jni\build") do set "EXPECTED_BUILD=%%~fD"
    if /i not "!RESOLVED_BUILD!"=="!EXPECTED_BUILD!" (
        echo [ERROR] Refusing to clean an unexpected directory: !RESOLVED_BUILD!
        set "BUILD_RESULT=1"
        goto :finish
    )
    if exist "!RESOLVED_BUILD!" rmdir /s /q "!RESOLVED_BUILD!"
)

set "NDK_CMAKE=%NDK:\=/%"
set "NINJA_CMAKE=%NINJA:\=/%"
echo [NDK] %NDK%
echo [TARGET] !TARGET!
echo [PROJECTILE_TRACKING] !PROJECTILE_TRACKING!

set "AUTH_PRELOAD="
if defined AUTH_CONFIG (
    set "AUTH_PRELOAD=-C "!AUTH_CONFIG!""
    echo [AUTH] Private configuration enabled.
) else (
    echo [AUTH] Runtime authentication uses the CMake cache/defaults.
)

cmake -Wno-deprecated --no-warn-unused-cli -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G Ninja ^
    !AUTH_PRELOAD! ^
    -DCMAKE_MAKE_PROGRAM="%NINJA_CMAKE%" ^
    -DCMAKE_TOOLCHAIN_FILE="%NDK_CMAKE%/build/cmake/android.toolchain.cmake" ^
    -DANDROID_ABI=arm64-v8a ^
    -DANDROID_PLATFORM=android-21 ^
    -DANDROID_STL=c++_static ^
    -DLENGJING_ENABLE_PROJECTILE_TRACKING=!PROJECTILE_TRACKING! ^
    -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo [ERROR] Android configuration failed.
    set "BUILD_RESULT=1"
    goto :finish
)

cmake --build "%BUILD_DIR%" --target "!TARGET!" --parallel 8
if errorlevel 1 (
    echo [ERROR] Android build failed.
    set "BUILD_RESULT=1"
    goto :finish
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
    set "BUILD_RESULT=1"
    goto :finish
)

for %%I in ("!BIN!") do echo [DONE] %%~fI ^(%%~zI bytes^)
set "BUILD_RESULT=0"
goto :finish

:probe_sdk_ndk
set "_NDK_BASE=%LOCALAPPDATA%\Android\Sdk\ndk"
if not exist "%_NDK_BASE%" exit /b 0
for /f "delims=" %%D in ('dir /B /AD /O-N "%_NDK_BASE%" 2^>nul') do (
    if not defined NDK (
        if exist "%_NDK_BASE%\%%D\build\cmake\android.toolchain.cmake" (
            set "NDK=%_NDK_BASE%\%%D"
        )
    )
)
exit /b 0

:finish
if "!PAUSE_ON_EXIT!"=="1" (
    echo.
    if "!BUILD_RESULT!"=="0" (
        echo [RESULT] Build succeeded.
    ) else (
        echo [RESULT] Build failed with exit code !BUILD_RESULT!.
    )
    echo Press any key to close this window...
    pause >nul
)
endlocal & exit /b %BUILD_RESULT%
