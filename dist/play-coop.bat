@echo off
rem Alien: Isolation co-op launcher. Steam must be running.
rem Over Steam (no IP, no port forwarding; you need different Steam accounts):
rem   play-coop.bat steam-host [name]              your Steam ID shows on screen; send it to your friend
rem   play-coop.bat steam-join <host SteamID64> [name]
rem Over UDP (direct IP):
rem   play-coop.bat host [name]                    friends join your IP, UDP port 45500
rem   play-coop.bat join <host-ip> [name]
setlocal
cd /d "%~dp0"
if /i "%1"=="steam-host" (
  set AIC_COOP_MODE=steam-host
  if not "%2"=="" set AIC_COOP_NAME=%2
  echo Hosting over Steam. Your Steam ID appears on screen and in coop_logs\my_steam_id.txt.
  echo Your friend runs: play-coop.bat steam-join ^<your Steam ID^>
) else if /i "%1"=="steam-join" (
  if "%2"=="" goto usage
  set AIC_COOP_MODE=steam-join
  set AIC_COOP_PEER_STEAM=%2
  if not "%3"=="" set AIC_COOP_NAME=%3
  echo Joining Steam user %2
) else if /i "%1"=="host" (
  set AIC_COOP_MODE=host
  if not "%2"=="" set AIC_COOP_NAME=%2
  echo Hosting on UDP port 45500. Your friend runs: play-coop.bat join ^<your IP^>
) else if /i "%1"=="join" (
  if "%2"=="" goto usage
  set AIC_COOP_MODE=join
  set AIC_COOP_PEER=%2:45500
  if not "%3"=="" set AIC_COOP_NAME=%3
  echo Joining %2:45500
) else goto usage
rem Skin (how your friend sees you): put a name like SAMUELS in skin.txt. Skins are experimental: set AIC_SKINS=1 to try them.
if exist "%~dp0skin.txt" set /p AIC_SKIN=<"%~dp0skin.txt"
start "" "%~dp0AI.exe"
exit /b 0
:usage
echo usage: play-coop.bat steam-host [name]
echo        play-coop.bat steam-join ^<host SteamID64^> [name]
echo        play-coop.bat host [name]
echo        play-coop.bat join ^<host-ip^> [name]
exit /b 1
