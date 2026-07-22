@echo off
chcp 65001 >nul 2>&1
setlocal EnableExtensions EnableDelayedExpansion

set "SOURCE_DIR=%~dp0jni"
set "BUILD_DIR=%SOURCE_DIR%\build"
set "TARGET="
set "PRODUCT="
set "BIN="
set "ROOT_BIN="
set "USE_UPX=0"
set "READELF="
set "SYMBOL_REPORT=%BUILD_DIR%\lengjing_symbol_check_%RANDOM%_%RANDOM%.txt"
set "RELEASE_REPORT=%BUILD_DIR%\lengjing_release_check_%RANDOM%_%RANDOM%.txt"

if /i "%~1"=="upx" set "USE_UPX=1"
if not "%~1"=="" if /i not "%~1"=="upx" (
    echo [ERROR] Unsupported mode: %~1
    echo [USAGE] 2_pack.bat [upx]
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

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [ERROR] CMake cache not found. Run 1_build.bat first.
    exit /b 1
)
findstr /B /C:"LENGJING_ENABLE_ALGORITHM_COORDINATE:BOOL=OFF" "%BUILD_DIR%\CMakeCache.txt" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Algorithm coordinate code is not disabled in this build.
    exit /b 1
)
if not exist "%BUILD_DIR%\compile_commands.json" (
    echo [ERROR] Compile command database not found. Run 1_build.bat first.
    exit /b 1
)
findstr /C:"LENGJING_ENABLE_ALGORITHM_COORDINATE=1" "%BUILD_DIR%\compile_commands.json" >nul 2>&1
if not errorlevel 1 (
    echo [ERROR] Algorithm coordinate code is enabled in the compile commands.
    exit /b 1
)

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
    "!READELF!" --symbols --wide "!BIN!" >"!SYMBOL_REPORT!" 2>nul
    if errorlevel 1 (
        del /q "!SYMBOL_REPORT!" >nul 2>&1
        echo [ERROR] Unable to inspect the build product symbols.
        exit /b 1
    )
    findstr /I /C:"AlgorithmCoordinate" /C:"RuntimeCoordinateCodec" /C:"ReadAlgorithmCoordinate" "!SYMBOL_REPORT!" >nul 2>&1
    if not errorlevel 1 (
        del /q "!SYMBOL_REPORT!" >nul 2>&1
        echo [ERROR] Algorithm coordinate symbols are present in the build product.
        exit /b 1
    )
    del /q "!SYMBOL_REPORT!" >nul 2>&1
echo [SYMBOLS] ELF symbol table verified.

set "UPX_EXE="
if "!USE_UPX!"=="1" (
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
)

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

if "!USE_UPX!"=="1" (
    for %%I in ("!ROOT_BIN!") do echo [UPX] before: %%~zI bytes
    "!UPX_EXE!" -t "!ROOT_BIN!" >nul 2>&1
    if not errorlevel 1 (
        echo [UPX] already packed and verified.
    ) else (
        "!UPX_EXE!" --best --lzma --no-progress "!ROOT_BIN!"
        if errorlevel 1 (
            echo [ERROR] UPX packing failed.
            del /q "!ROOT_BIN!" >nul 2>&1
            exit /b 1
        )
    )
    "!UPX_EXE!" -t "!ROOT_BIN!" >nul 2>&1
    if errorlevel 1 (
        echo [ERROR] Packed product verification failed.
        del /q "!ROOT_BIN!" >nul 2>&1
        exit /b 1
    )
    for %%I in ("!ROOT_BIN!") do echo [UPX] after: %%~zI bytes
) else (
    echo [SYMBOLS] Unstripped CMake product copied without compression.
)

"!READELF!" --sections --wide "!ROOT_BIN!" >"!RELEASE_REPORT!" 2>nul
if errorlevel 1 (
    del /q "!RELEASE_REPORT!" "!ROOT_BIN!" >nul 2>&1
    echo [ERROR] Unable to inspect the release product.
    exit /b 1
)
findstr /C:".symtab" "!RELEASE_REPORT!" >nul 2>&1
if errorlevel 1 (
    del /q "!RELEASE_REPORT!" "!ROOT_BIN!" >nul 2>&1
    echo [ERROR] The release product lost its ELF symbol table.
    exit /b 1
)
"!READELF!" --symbols --wide "!ROOT_BIN!" >"!RELEASE_REPORT!" 2>nul
if errorlevel 1 (
    del /q "!RELEASE_REPORT!" "!ROOT_BIN!" >nul 2>&1
    echo [ERROR] Unable to inspect the release product symbols.
    exit /b 1
)
findstr /I /C:"AlgorithmCoordinate" /C:"RuntimeCoordinateCodec" /C:"ReadAlgorithmCoordinate" "!RELEASE_REPORT!" >nul 2>&1
if not errorlevel 1 (
    del /q "!RELEASE_REPORT!" "!ROOT_BIN!" >nul 2>&1
    echo [ERROR] Algorithm coordinate symbols are present in the release product.
    exit /b 1
)
del /q "!RELEASE_REPORT!" >nul 2>&1
echo [SYMBOLS] Release ELF symbol table preserved.

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
    echo [ERROR] The build product changed during packing.
    exit /b 1
)
if "!USE_UPX!"=="0" if /i not "!BUILD_SHA_BEFORE!"=="!ROOT_SHA!" (
    echo [ERROR] The copied release product differs from the CMake product.
    exit /b 1
)
echo [DONE] !ROOT_BIN!
echo [SHA256] !ROOT_SHA!
exit /b 0
