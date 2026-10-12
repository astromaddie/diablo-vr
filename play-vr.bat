@echo off
rem Starts the game in the headset. Have your OpenXR runtime (SteamVR, Quest Link, Virtual Desktop)
rem running and the headset connected first; the game also picks the headset up if it appears later.
rem A log is written to vr.log next to the game.
setlocal
set "GAME=%~dp0"
if not exist "%GAME%devilutionx.exe" set "GAME=%~dp0build\Release\"
if not exist "%GAME%devilutionx.exe" (
	echo devilutionx.exe was not found. Build it first with build-vr.bat.
	pause
	exit /b 1
)
if not exist "%GAME%DIABDAT.MPQ" if not exist "%GAME%diabdat.mpq" if not exist "%APPDATA%\diasurgical\devilution\DIABDAT.MPQ" (
	echo DIABDAT.MPQ was not found. Copy it from your own copy of Diablo to:
	echo   %GAME%
	pause
	exit /b 1
)
cd /d "%GAME%"
start "" devilutionx.exe --vr --verbose --log-to-file "%GAME%vr.log" %*
