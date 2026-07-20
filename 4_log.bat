@echo off
chcp 65001 >nul 2>&1
setlocal EnableExtensions EnableDelayedExpansion

set "SOURCE_DIR=%~dp0jni"
set "STATE_DIR=E:\demo\fenxi\lengjing\_misc"
set "SERIAL_CACHE=%STATE_DIR%\adb_serial.txt"
set "DEVLIST=%STATE_DIR%\adb_devices_%RANDOM%_%RANDOM%.txt"
set "TARGET="

if not exist "%STATE_DIR%" mkdir "%STATE_DIR%"
if errorlevel 1 (
    echo [ERROR] Unable to create device state directory: %STATE_DIR%
    exit /b 1
)
for /f "tokens=2 delims=( " %%T in ('findstr /R /I /C:"^[ ]*add_executable(" "%SOURCE_DIR%\CMakeLists.txt"') do (
    if not defined TARGET set "TARGET=%%T"
)
if not defined TARGET set "TARGET=lengjing"
set "TARGET=!TARGET:"=!"

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
"%ADB%" -s "!SERIAL!" logcat -s "!TARGET!"
exit /b !errorlevel!
