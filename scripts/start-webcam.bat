@echo off
adb devices | findstr /r "device$" >nul
if %errorlevel% neq 0 (
    echo No device connected
    exit /b 1
)
adb shell input keyevent KEYCODE_WAKEUP
timeout /t 1 /nobreak >nul
adb shell input swipe 540 1800 540 800
timeout /t 1 /nobreak >nul
adb shell input text 1234
adb shell input keyevent 66
timeout /t 2 /nobreak >nul
adb shell monkey -p com.dev47apps.obsdroidcam -c android.intent.category.LAUNCHER 1
timeout /t 2 /nobreak >nul
cd "C:\Program Files\DroidCam\Client\bin\64bit"
start /min "" "C:\Program Files\DroidCam\Client\bin\64bit\droidcam.exe"
echo Webcam ready