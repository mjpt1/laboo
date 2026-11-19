# Puts the board back exactly as it was on 2026-11-01 (Laboo firmware, full 16 MB image).
# Usage (PowerShell, in this folder):
#     powershell -ExecutionPolicy Bypass -File .\restore.ps1          # finds the board
#     powershell -ExecutionPolicy Bypass -File .\restore.ps1 COM8     # or give the port
# If it won't connect: hold BOOT, press RESET, release BOOT, run again.

$img = Join-Path $PSScriptRoot "laboo_full_16MB.bin"
$port = $args[0]
if (-not $port) {
    foreach ($d in Get-CimInstance Win32_PnPEntity) {
        if ($d.PNPDeviceID -match "VID_303A" -and $d.Name -match "\((COM\d+)\)") { $port = $Matches[1]; break }
    }
}
if (-not $port) { Write-Host "Board not found." -ForegroundColor Red; exit 1 }
python -m esptool --chip esp32s3 --port $port --baud 921600 write_flash 0 $img
