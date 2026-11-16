@echo off
setlocal
cd /d "%~dp0"

where node >nul 2>nul
if errorlevel 1 (
  echo Node.js is required to build the Android app.
  goto :failed
)

where npm >nul 2>nul
if errorlevel 1 (
  echo npm is required to build the Android app.
  goto :failed
)

where java >nul 2>nul
if errorlevel 1 (
  echo Java is required to build the Android app.
  goto :failed
)

if not defined ANDROID_HOME if defined ANDROID_SDK_ROOT set "ANDROID_HOME=%ANDROID_SDK_ROOT%"
if not defined ANDROID_HOME (
  echo Set ANDROID_HOME or ANDROID_SDK_ROOT to your Android SDK folder.
  goto :failed
)

if not exist "%ANDROID_HOME%\platforms" (
  echo Android SDK platforms were not found in "%ANDROID_HOME%".
  goto :failed
)

if not exist "resources\icon.png" (
  echo Android app icon source was not found at resources\icon.png.
  goto :failed
)

if not exist "resources\splash.png" (
  echo Android splash screen source was not found at resources\splash.png.
  goto :failed
)

if not exist "node_modules\@capacitor\android\package.json" (
  echo Installing project dependencies...
  call npm ci
  if errorlevel 1 goto :failed
)

if not exist "node_modules\@capacitor\assets\package.json" (
  echo Installing project dependencies...
  call npm ci
  if errorlevel 1 goto :failed
)

echo Building web assets...
call npm run build
if errorlevel 1 goto :failed

if not exist "android\gradlew.bat" (
  echo Creating the Capacitor Android project...
  call npx cap add android
  if errorlevel 1 goto :failed
)

echo Generating Android icons and splash screen from resources...
call npx capacitor-assets generate --android --assetPath resources
if errorlevel 1 goto :failed

echo Syncing web assets and Capacitor plugins...
call npx cap sync android
if errorlevel 1 goto :failed

echo Building debug APK...
pushd android
call gradlew.bat assembleDebug
if errorlevel 1 (
  popd
  goto :failed
)
popd

set "APK_PATH=%~dp0android\app\build\outputs\apk\debug\app-debug.apk"
if not exist "%APK_PATH%" (
  echo Gradle completed, but the APK was not found at:
  echo %APK_PATH%
  goto :failed
)

echo.
echo APK build complete:
echo %APK_PATH%
exit /b 0

:failed
echo.
echo Android APK build failed. Review the error above.
exit /b 1
