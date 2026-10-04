@echo off
rem Starts the CastKu system helper on the phone (needed once after every phone reboot).
rem Requirements: USB debugging enabled, phone connected, the app installed.
setlocal

where adb >nul 2>nul
if %errorlevel%==0 (
  set "ADB=adb"
) else (
  set "ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe"
)

"%ADB%" get-state >nul 2>nul
if not %errorlevel%==0 (
  echo No phone found. Connect it with USB and allow USB debugging.
  exit /b 1
)

rem Everything runs on the phone in one shell: find the installed APK and launch the helper from it.
"%ADB%" shell "pkill -f '[H]elperMain'; APK=$(pm path com.xcast.display | cut -d: -f2); [ -z $APK ] && echo 'CastKu is not installed on the phone.' && exit 1; CLASSPATH=$APK setsid app_process / com.xcast.helper.HelperMain --app com.xcast.display </dev/null >/data/local/tmp/xcast-helper.log 2>&1 & sleep 2; head -3 /data/local/tmp/xcast-helper.log"
if not %errorlevel%==0 exit /b 1

echo.
echo Helper started. Open CastKu on the phone and tap "Aktifkan".
endlocal
