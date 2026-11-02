@echo off
rem Alien: Isolation co-op launcher. Steam must be running.
rem   play-coop.bat host [name]            you host: friends join your IP, UDP port 45500
rem   play-coop.bat join <host-ip> [name]  join a friend who is hosting
setlocal
cd /d "%~dp0"
if /i "%1"=="host" (
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
start "" "%~dp0AI.exe"
exit /b 0
:usage
echo usage: play-coop.bat host [name]
echo        play-coop.bat join ^<host-ip^> [name]
exit /b 1
