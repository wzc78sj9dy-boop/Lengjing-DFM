@echo off
chcp 65001 >nul 2>&1
setlocal EnableExtensions EnableDelayedExpansion

set "SOURCE_DIR=%~dp0jni"
set "BUILD_DIR=%SOURCE_DIR%\build"
set "TARGET="
set "PRODUCT="
set "BIN="
set "ROOT_BIN="

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

if exist "%BUILD_DIR%\!PRODUCT!" set "BIN=%BUILD_DIR%\!PRODUCT!"
if not defined BIN if exist "%BUILD_DIR%\build.ninja" (
    for /f "tokens=2" %%F in ('findstr /R /C:"^build .*: CXX_EXECUTABLE_LINKER" "%BUILD_DIR%\build.ninja"') do (
        if not defined LINK_OUTPUT set "LINK_OUTPUT=%%F"
    )
    if defined LINK_OUTPUT (
        set "LINK_OUTPUT=!LINK_OUTPUT:~0,-1!"
        set "LINK_OUTPUT=!LINK_OUTPUT:/=\!"
        if exist "%BUILD_DIR%\!LINK_OUTPUT!" set "BIN=%BUILD_DIR%\!LINK_OUTPUT!"
    )
)
if not defined BIN (
    echo [ERROR] Product not found. Run 1_build.bat first.
    exit /b 1
)
set "ROOT_BIN=%~dp0!PRODUCT!"

set "UPX_EXE="
if defined UPX_PATH (
    if exist "%UPX_PATH%\upx.exe" set "UPX_EXE=%UPX_PATH%\upx.exe"
    if not defined UPX_EXE if exist "%UPX_PATH%" set "UPX_EXE=%UPX_PATH%"
)
if not defined UPX_EXE for %%P in (
    "D:\tools\upx\upx.exe"
    "C:\upx-5.0.2-win64\upx.exe"
    "C:\upx\upx.exe"
    "D:\upx\upx.exe"
    "D:\AAA\upx\upx.exe"
    "%USERPROFILE%\tools\upx\upx.exe"
) do (
    if not defined UPX_EXE if exist "%%~P" set "UPX_EXE=%%~P"
)
if not defined UPX_EXE for %%I in (upx.exe) do set "UPX_EXE=%%~$PATH:I"

if not defined UPX_EXE (
    echo [ERROR] upx.exe not found. Set UPX_PATH or add it to PATH.
    exit /b 1
)

for %%I in ("!BIN!") do echo [UPX] before: %%~zI bytes
"!UPX_EXE!" -t "!BIN!" >nul 2>&1
if not errorlevel 1 (
    echo [UPX] already packed and verified.
) else (
    "!UPX_EXE!" --best --lzma --no-progress "!BIN!"
    if errorlevel 1 (
        echo [ERROR] UPX packing failed.
        exit /b 1
    )
)
"!UPX_EXE!" -t "!BIN!" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Packed product verification failed.
    exit /b 1
)
for %%I in ("!BIN!") do echo [UPX] after: %%~zI bytes

copy /y "!BIN!" "!ROOT_BIN!" >nul
if errorlevel 1 (
    echo [ERROR] Unable to synchronize the packed product.
    exit /b 1
)

set "BUILD_SHA="
set "ROOT_SHA="
for /f "usebackq delims=" %%H in (`powershell.exe -NoProfile -NonInteractive -Command "(Get-FileHash -LiteralPath $env:BIN -Algorithm SHA256).Hash.ToLowerInvariant()"`) do set "BUILD_SHA=%%H"
for /f "usebackq delims=" %%H in (`powershell.exe -NoProfile -NonInteractive -Command "(Get-FileHash -LiteralPath $env:ROOT_BIN -Algorithm SHA256).Hash.ToLowerInvariant()"`) do set "ROOT_SHA=%%H"
if not defined BUILD_SHA (
    echo [ERROR] Unable to hash the build product.
    exit /b 1
)
if not defined ROOT_SHA (
    echo [ERROR] Unable to hash the synchronized product.
    exit /b 1
)
if /i not "!BUILD_SHA!"=="!ROOT_SHA!" (
    echo [ERROR] Build and synchronized product hashes differ.
    exit /b 1
)

echo [DONE] !ROOT_BIN!
echo [SHA256] !BUILD_SHA!
exit /b 0
