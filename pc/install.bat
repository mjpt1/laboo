@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo === LABOO setup ===
if not exist .venv\Scripts\python.exe (
  echo [1/4] Creating Python environment...
  python -m venv .venv
  if errorlevel 1 (
    echo Python not found. Install Python 3.11 or 3.12 from python.org and tick "Add python.exe to PATH".
    pause
    exit /b 1
  )
)
set PIP=.venv\Scripts\python -m pip install --default-timeout 60 --retries 10 --progress-bar on
echo.
echo [2/4] Small packages (serial port, Claude, voice)...
%PIP% pyserial numpy pyyaml anthropic edge-tts miniaudio sounddevice openai vosk opencv-python pillow arabic-reshaper python-bidi
if errorlevel 1 goto fail
echo.
echo [3/4] Whisper speech recognition (the big one, ~150 MB, please wait)...
%PIP% faster-whisper
if errorlevel 1 goto fail
echo.
echo [4/4] Checking...
.venv\Scripts\python -c "import serial, numpy, yaml, anthropic, edge_tts, miniaudio, sounddevice, faster_whisper, vosk, cv2, PIL, arabic_reshaper, bidi; print('All packages OK')"
if errorlevel 1 goto fail
echo.
echo === Done. Now run run.bat ===
pause
exit /b 0

:fail
echo.
echo *** Install failed. Usually the internet / VPN. Run install.bat again - finished parts are kept. ***
pause
exit /b 1
