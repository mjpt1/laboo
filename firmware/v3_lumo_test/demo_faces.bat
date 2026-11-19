@echo off
cd /d "%~dp0"
py -3 demo_faces.py 2>nul || python demo_faces.py
pause
