@echo off
chcp 65001 >nul
cd /d "%~dp0"
.venv\Scripts\python -m pip install --default-timeout 60 --retries 10 opencv-python
.venv\Scripts\python camera.py %1
pause
