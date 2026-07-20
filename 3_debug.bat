@echo off
chcp 65001 >nul 2>&1
setlocal EnableExtensions EnableDelayedExpansion

set "SOURCE_DIR=%~dp0jni"
set "BUILD_DIR=%SOURCE_DIR%\build"
set "REMOTE_DIR=/data/local/tmp"
set "STATE_DIR=E:\demo\fenxi\lengjing\_misc"
set "SERIAL_CACHE=%STATE_DIR%\adb_serial.txt"
set "DEVLIST=%STATE_DIR%\adb_devices_%RANDOM%_%RANDOM%.txt"
set "CHECK_FILE=%STATE_DIR%\adb_push_%RANDOM%_%RANDOM%.txt"
set "TARGET="
set "PRODUCT="
set "REMOTE_PRODUCT=.lj_app"
set "BIN="

if not exist "%STATE_DIR%" mkdir "%STATE_DIR%"
if errorlevel 1 (
    echo [ERROR] Unable to create device state directory: %STATE_DIR%
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
        if exist "%BUILD_DIR%\!LINK_OUTPUT!" (
            set "BIN=%BUILD_DIR%\!LINK_OUTPUT!"
            for %%I in ("!BIN!") do set "PRODUCT=%%~nxI"
        )
    )
)
if not defined BIN (
    echo [ERROR] Product not found. Run 1_build.bat first.
    exit /b 1
)
call "%~dp02_pack.bat"
if errorlevel 1 (
    echo [ERROR] Unable to prepare the release product.
    exit /b 1
)
set "BIN=%~dp0!PRODUCT!"
for %%I in ("!BIN!") do set "LOCAL_SIZE=%%~zI"

set "ADB="
if defined ADB_PATH (
    if exist "%ADB_PATH%\adb.exe" set "ADB=%ADB_PATH%\adb.exe"
    if not defined ADB if exist "%ADB_PATH%" set "ADB=%ADB_PATH%"
)
if not defined ADB if exist "%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe" set "ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe"
if not defined ADB if exist "D:\AAA\platform-tools\adb.exe" set "ADB=D:\AAA\platform-tools\adb.exe"
if not defined ADB for %%I in (adb.exe) do set "ADB=%%~$PATH:I"
if not defined ADB (
    echo [ERROR] adb.exe not found. Set ADB_PATH or add it to PATH.
    exit /b 1
)

"%ADB%" start-server >nul 2>&1
set "SERIAL="
if defined ADB_SERIAL (
    "%ADB%" -s "!ADB_SERIAL!" get-state >"%DEVLIST%" 2>nul
    set "STATE=offline"
    for /f "usebackq delims=" %%S in ("%DEVLIST%") do set "STATE=%%S"
    if /i "!STATE!"=="device" set "SERIAL=!ADB_SERIAL!"
)
if not defined SERIAL if exist "%SERIAL_CACHE%" (
    set /p "CACHED_SERIAL="<"%SERIAL_CACHE%"
    if defined CACHED_SERIAL (
        "%ADB%" -s "!CACHED_SERIAL!" get-state >"%DEVLIST%" 2>nul
        set "STATE=offline"
        for /f "usebackq delims=" %%S in ("%DEVLIST%") do set "STATE=%%S"
        if /i "!STATE!"=="device" set "SERIAL=!CACHED_SERIAL!"
    )
)

if not defined SERIAL (
    "%ADB%" devices >"%DEVLIST%" 2>nul
    set "DEVICE_COUNT=0"
    for /f "usebackq skip=1 tokens=1,2" %%A in ("%DEVLIST%") do (
        if /i "%%B"=="device" (
            echo %%A | findstr /C:":" >nul
            if errorlevel 1 (
                set /a DEVICE_COUNT+=1
                set "DEVICE_!DEVICE_COUNT!=%%A"
            )
        )
    )
    if !DEVICE_COUNT! equ 0 (
        echo [ERROR] No online USB Android device found.
        type "%DEVLIST%"
        del /q "%DEVLIST%" >nul 2>&1
        exit /b 1
    )
    set "SERIAL=!DEVICE_1!"
    if !DEVICE_COUNT! gtr 1 (
        echo [ADB] Multiple USB devices:
        for /l %%I in (1,1,!DEVICE_COUNT!) do echo   %%I^) !DEVICE_%%I!
        set "PICK="
        set /p "PICK=Select device index [1]: "
        if defined PICK (
            set "SELECTED="
            for /l %%I in (1,1,!DEVICE_COUNT!) do if "!PICK!"=="%%I" set "SELECTED=!DEVICE_%%I!"
            if defined SELECTED set "SERIAL=!SELECTED!"
        )
    )
)
>"%SERIAL_CACHE%" echo !SERIAL!
del /q "%DEVLIST%" >nul 2>&1

echo [ADB] device selected
"%ADB%" -s "!SERIAL!" root >nul 2>&1
"%ADB%" -s "!SERIAL!" wait-for-device
if errorlevel 1 (
    echo [ERROR] Device did not return online after root.
    exit /b 1
)

"%ADB%" -s "!SERIAL!" shell "su -c 'pkill -TERM -x !REMOTE_PRODUCT! 2>/dev/null || true; pkill -TERM -x !PRODUCT! 2>/dev/null || true; pkill -TERM -x !PRODUCT!.perf 2>/dev/null || true; n=0; while { pidof !REMOTE_PRODUCT! >/dev/null 2>&1 || pidof !PRODUCT! >/dev/null 2>&1 || pidof !PRODUCT!.perf >/dev/null 2>&1; } && [ $n -lt 20 ]; do sleep 0.1; n=$((n+1)); done; pkill -KILL -x !REMOTE_PRODUCT! 2>/dev/null || true; pkill -KILL -x !PRODUCT! 2>/dev/null || true; pkill -KILL -x !PRODUCT!.perf 2>/dev/null || true; rm -f %REMOTE_DIR%/!REMOTE_PRODUCT! %REMOTE_DIR%/!PRODUCT! %REMOTE_DIR%/!PRODUCT!.perf'" >nul 2>&1
set "PUSH_TRY=0"
:push_retry
set /a PUSH_TRY+=1
"%ADB%" -s "!SERIAL!" push "!BIN!" "%REMOTE_DIR%/!REMOTE_PRODUCT!"
set "PUSH_RESULT=!errorlevel!"
"%ADB%" -s "!SERIAL!" shell "su -c 'if [ -f %REMOTE_DIR%/!REMOTE_PRODUCT! ]; then wc -c < %REMOTE_DIR%/!REMOTE_PRODUCT!; else echo MISS; fi'" >"%CHECK_FILE%" 2>nul
if "!PUSH_RESULT!"=="0" (
    findstr /R /X /C:" *!LOCAL_SIZE! *" "%CHECK_FILE%" >nul 2>&1
    if not errorlevel 1 goto pushed
)
if !PUSH_TRY! lss 3 (
    echo [WARN] Push failed with exit code !PUSH_RESULT!, retry !PUSH_TRY!/3.
    timeout /t 1 /nobreak >nul
    goto push_retry
)
del /q "%CHECK_FILE%" >nul 2>&1
echo [ERROR] Push failed after 3 attempts.
exit /b 1

:pushed
del /q "%CHECK_FILE%" >nul 2>&1
"%ADB%" -s "!SERIAL!" shell "su -c 'chmod 700 %REMOTE_DIR%/!REMOTE_PRODUCT!'"
if errorlevel 1 (
    echo [ERROR] Unable to make the remote product executable.
    exit /b 1
)

set "RUN_SERIAL=!SERIAL!"
set "RUN_PRODUCT=!REMOTE_PRODUCT!"
setlocal DisableDelayedExpansion
echo [RUN] helper
"%ADB%" -s "%RUN_SERIAL%" shell -tt "su -c 'cd %REMOTE_DIR% && exec ./%RUN_PRODUCT%'"
set "RUN_RESULT=%errorlevel%"
endlocal & exit /b %RUN_RESULT%
