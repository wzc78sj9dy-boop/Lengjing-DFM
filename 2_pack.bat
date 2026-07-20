@echo off
chcp 65001 >nul 2>&1
setlocal EnableExtensions EnableDelayedExpansion

set "SOURCE_DIR=%~dp0jni"
set "BUILD_DIR=%SOURCE_DIR%\build"
set "TARGET="
set "PRODUCT="
set "BIN="
set "ROOT_BIN="
set "READELF="
set "SYMBOL_REPORT=%BUILD_DIR%\lengjing_symbol_check_%RANDOM%_%RANDOM%.txt"

if not "%~1"=="" (
    echo [ERROR] Unsupported argument: %~1
    echo [USAGE] 2_pack.bat
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

for %%I in (llvm-readelf.exe) do set "READELF=%%~$PATH:I"
if not defined READELF if exist "%BUILD_DIR%\CMakeCache.txt" (
    for /f "tokens=2 delims==" %%R in ('findstr /B /C:"CMAKE_READELF:FILEPATH=" "%BUILD_DIR%\CMakeCache.txt"') do (
        if exist "%%R" set "READELF=%%R"
    )
)
if not defined READELF if exist "%BUILD_DIR%\CMakeCache.txt" (
    for /f "tokens=2 delims==" %%C in ('findstr /B /C:"CMAKE_CXX_COMPILER:FILEPATH=" "%BUILD_DIR%\CMakeCache.txt"') do (
        for %%D in ("%%C") do if exist "%%~dpDllvm-readelf.exe" set "READELF=%%~dpDllvm-readelf.exe"
    )
)
if not defined READELF (
    echo [ERROR] llvm-readelf.exe not found. Cannot verify the symbol table.
    exit /b 1
)
"!READELF!" --sections --wide "!BIN!" >"!SYMBOL_REPORT!" 2>nul
if errorlevel 1 (
    del /q "!SYMBOL_REPORT!" >nul 2>&1
    echo [ERROR] Unable to inspect the build product symbol table.
    exit /b 1
)
findstr /C:".symtab" "!SYMBOL_REPORT!" >nul 2>&1
if errorlevel 1 (
    del /q "!SYMBOL_REPORT!" >nul 2>&1
    echo [ERROR] The build product is stripped: .symtab is missing.
    exit /b 1
)
del /q "!SYMBOL_REPORT!" >nul 2>&1
echo [SYMBOLS] ELF symbol table verified.

set "BUILD_SHA_BEFORE="
set "BUILD_SHA_AFTER="
set "ROOT_SHA="
for /f "usebackq delims=" %%H in (`powershell.exe -NoProfile -NonInteractive -Command "(Get-FileHash -LiteralPath $env:BIN -Algorithm SHA256).Hash.ToLowerInvariant()"`) do set "BUILD_SHA_BEFORE=%%H"
if not defined BUILD_SHA_BEFORE (
    echo [ERROR] Unable to hash the build product.
    exit /b 1
)

copy /y "!BIN!" "!ROOT_BIN!" >nul
if errorlevel 1 (
    echo [ERROR] Unable to create the release product.
    exit /b 1
)

echo [SYMBOLS] Unstripped build copied without compression.

for /f "usebackq delims=" %%H in (`powershell.exe -NoProfile -NonInteractive -Command "(Get-FileHash -LiteralPath $env:BIN -Algorithm SHA256).Hash.ToLowerInvariant()"`) do set "BUILD_SHA_AFTER=%%H"
for /f "usebackq delims=" %%H in (`powershell.exe -NoProfile -NonInteractive -Command "(Get-FileHash -LiteralPath $env:ROOT_BIN -Algorithm SHA256).Hash.ToLowerInvariant()"`) do set "ROOT_SHA=%%H"
if not defined BUILD_SHA_AFTER (
    echo [ERROR] Unable to verify the build product.
    exit /b 1
)
if not defined ROOT_SHA (
    echo [ERROR] Unable to hash the release product.
    exit /b 1
)
if /i not "!BUILD_SHA_BEFORE!"=="!BUILD_SHA_AFTER!" (
    echo [ERROR] The build product changed while creating the release copy.
    exit /b 1
)
if /i not "!BUILD_SHA_AFTER!"=="!ROOT_SHA!" (
    echo [ERROR] The unstripped release copy does not match the build product.
    exit /b 1
)

echo [DONE] !ROOT_BIN!
echo [SHA256] !ROOT_SHA!
exit /b 0
