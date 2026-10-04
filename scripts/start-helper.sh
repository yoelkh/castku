#!/usr/bin/env sh
# Starts the CastKu system helper on the phone (needed once after every phone reboot).
# Requirements: USB debugging enabled, phone connected, the app installed.
set -e
ADB="${ADB:-adb}"

if ! "$ADB" get-state >/dev/null 2>&1; then
  echo "No phone found. Connect it with USB and allow USB debugging." >&2
  exit 1
fi

# Everything runs on the phone in one shell: find the installed APK and launch the helper from it.
"$ADB" shell 'pkill -f "[H]elperMain"; APK=$(pm path com.xcast.display | cut -d: -f2)
if [ -z "$APK" ]; then echo "CastKu is not installed on the phone."; exit 1; fi
CLASSPATH=$APK setsid app_process / com.xcast.helper.HelperMain --app com.xcast.display \
  </dev/null >/data/local/tmp/xcast-helper.log 2>&1 &
sleep 2; head -3 /data/local/tmp/xcast-helper.log'

echo
echo 'Helper started. Open CastKu on the phone and tap "Aktifkan".'
