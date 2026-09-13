@echo off
setlocal

:: --- Paths (adjust to your machine) ---
:: Path to ninja.exe
set "NINJA_PATH=%LOCALAPPDATA%\Programs\Ninja\ninja.exe"
:: Path to MinGW-w64 tools (g++, cmake)
set "MINGW_GXX=C:\MinGW64\mingw64\bin\g++.exe"
set "MINGW_CMAKE=C:\MinGW64\mingw64\bin\cmake.exe"

set "PROJECT_ROOT=%~dp0"
set "LOG_DIR=%PROJECT_ROOT%logs"
set "LOG_FILE=%LOG_DIR%static_build.log"

:: --- Create log dir if needed ---
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"

echo [%date% %time%] Build started. > "%LOG_FILE%"

:: --- Configure CMake (static build: MinGW runtime statically linked) ---
echo Configuring CMake...
"%MINGW_CMAKE%" -S "%PROJECT_ROOT%." -B "%PROJECT_ROOT%build" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA_PATH%" -DCMAKE_CXX_COMPILER="%MINGW_GXX%" -DCMAKE_BUILD_TYPE=Release -DSAMP_STATIC=ON >> "%LOG_FILE%" 2>&1

if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configure failed with code %ERRORLEVEL% >> "%LOG_FILE%"
    echo CMake configure failed. Check the log: %LOG_FILE%
    pause
    exit /b %ERRORLEVEL%
)

:: --- Build ---
echo Building...
"%MINGW_CMAKE%" --build "%PROJECT_ROOT%build" --config Release >> "%LOG_FILE%" 2>&1

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed with code %ERRORLEVEL% >> "%LOG_FILE%"
    echo Build failed. Check the log: %LOG_FILE%
    pause
    exit /b %ERRORLEVEL%
)

:: --- Remove MinGW runtime DLLs (static build, they are not needed) ---
if exist "%PROJECT_ROOT%build\libstdc++-6.dll" del /Q "%PROJECT_ROOT%build\libstdc++-6.dll"
if exist "%PROJECT_ROOT%build\libgcc_s_seh-1.dll" del /Q "%PROJECT_ROOT%build\libgcc_s_seh-1.dll"
if exist "%PROJECT_ROOT%build\libwinpthread-1.dll" del /Q "%PROJECT_ROOT%build\libwinpthread-1.dll"

:: --- Result ---
echo.
echo ================================
echo Build finished. Binary: %PROJECT_ROOT%build\samp.exe
echo Log: %LOG_FILE%
echo ================================
echo [%date% %time%] Build finished. >> "%LOG_FILE%"
pause