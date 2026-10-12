@echo off
rem Configures and builds the VR version (Windows x64, Visual Studio 2022, CMake 3.22 or newer).
rem Every dependency is downloaded and built from source by CMake: nothing else to install.
rem Result: build\Release\devilutionx.exe
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
	echo CMake was not found on PATH. Install it from https://cmake.org/download/ or run this
	echo from the "x64 Native Tools Command Prompt for VS 2022", which has its own.
	exit /b 1
)

if not exist build\CMakeCache.txt (
	cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
		-DDEVILUTIONX_VR=ON ^
		-DBUILD_TESTING=OFF ^
		-DDISABLE_ZERO_TIER=ON ^
		-DDISCORD_INTEGRATION=OFF ^
		-DDEVILUTIONX_SYSTEM_SDL2=OFF ^
		-DDEVILUTIONX_SYSTEM_SDL_IMAGE=OFF ^
		-DDEVILUTIONX_SYSTEM_SDL_AUDIOLIB=OFF ^
		-DDEVILUTIONX_SYSTEM_ZLIB=OFF ^
		-DDEVILUTIONX_SYSTEM_BZIP2=OFF ^
		-DDEVILUTIONX_SYSTEM_LUA=OFF ^
		-DDEVILUTIONX_SYSTEM_LIBSODIUM=OFF ^
		-DDEVILUTIONX_SYSTEM_SHEENBIDI=OFF ^
		-DDEVILUTIONX_SYSTEM_MAGIC_ENUM=OFF
	if errorlevel 1 exit /b 1
)

cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1

rem The game looks for its own assets (fonts, UI art, data tables) beside the executable.
xcopy /e /i /y /q build\assets build\Release\assets >nul
if errorlevel 1 exit /b 1

echo.
echo Built build\Release\devilutionx.exe
echo Copy your DIABDAT.MPQ next to it, then run play-vr.bat with the headset connected.
