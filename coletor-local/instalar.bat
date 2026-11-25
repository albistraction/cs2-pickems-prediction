@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo === Instalando o coletor local do pickems-prediction ===
where node >nul 2>nul
if errorlevel 1 (
  echo.
  echo Node.js nao encontrado. Instale a versao LTS em https://nodejs.org e rode este arquivo de novo.
  pause
  exit /b 1
)
call npm install --no-audit --no-fund
if errorlevel 1 (
  echo Falha no npm install.
  pause
  exit /b 1
)
call npx playwright install chromium
if errorlevel 1 (
  echo Falha ao baixar o Chromium.
  pause
  exit /b 1
)
echo.
echo Pronto! Agora de dois cliques em coletar.bat
pause
