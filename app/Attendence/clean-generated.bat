@echo off
setlocal
cd /d "%~dp0"

echo This removes generated build outputs, Gradle project caches, and test reports.
echo It keeps source files, the Android project, node_modules, and local configuration.
echo.
echo Targets:
echo   dist
echo   android\app\build
echo   android\build
echo   android\.gradle
echo   android\capacitor-cordova-android-plugins\build
echo   coverage
echo   test-results
echo   playwright-report
echo   blob-report
echo.

choice /C YN /N /M "Delete these generated folders? [Y/N] "
if errorlevel 2 exit /b 0

call :clean_dir "dist"
if errorlevel 1 goto :failed
call :clean_dir "android\app\build"
if errorlevel 1 goto :failed
call :clean_dir "android\build"
if errorlevel 1 goto :failed
call :clean_dir "android\.gradle"
if errorlevel 1 goto :failed
call :clean_dir "android\capacitor-cordova-android-plugins\build"
if errorlevel 1 goto :failed
call :clean_dir "coverage"
if errorlevel 1 goto :failed
call :clean_dir "test-results"
if errorlevel 1 goto :failed
call :clean_dir "playwright-report"
if errorlevel 1 goto :failed
call :clean_dir "blob-report"
if errorlevel 1 goto :failed

echo.
echo Generated outputs cleaned.
exit /b 0

:clean_dir
if not exist "%~1\" exit /b 0
echo Removing %~1
rmdir /s /q "%~1"
if exist "%~1\" exit /b 1
exit /b 0

:failed
echo.
echo Could not remove one or more generated folders.
exit /b 1
