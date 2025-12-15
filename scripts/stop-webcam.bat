@echo off
taskkill /IM droidcam.exe /F >nul 2>&1
adb shell am force-stop com.dev47apps.obsdroidcam
adb shell input keyevent KEYCODE_SLEEP
echo Webcam stopped