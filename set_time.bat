@echo off
cd /d "%~dp0"
py -3 set_time.py 2>nul || python set_time.py
pause
