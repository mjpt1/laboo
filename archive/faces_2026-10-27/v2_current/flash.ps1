$APP = "laboo_face.bin"
# Writes the ready-made firmware (firmware\bin) to the ESP32-S3 board with esptool.
# No Arduino needed (downloads.arduino.cc is blocked from Iran).
# Usage (in PowerShell, inside the laboo folder):
#     powershell -ExecutionPolicy Bypass -File .\flash.ps1          # finds the board by itself
#     powershell -ExecutionPolicy Bypass -File .\flash.ps1 COM5     # or give the port

$bin = $PSScriptRoot
foreach ($f in "bootloader.bin", "partitions.bin", "boot_app0.bin", $APP) {
    if (-not (Test-Path (Join-Path $bin $f))) { Write-Host "Missing firmware\bin\$f" -ForegroundColor Red; exit 1 }
}

# 1) find Python (the robot's own venv first)
function Test-Py($cmd, $extra) {
    try { & $cmd @extra --version 2>$null | Out-Null; return $LASTEXITCODE -eq 0 } catch { return $false }
}
$py = $null; $pyArgs = @()
$venv = Join-Path $PSScriptRoot "..\..\..\pc\.venv\Scripts\python.exe"
if ((Test-Path $venv) -and (Test-Py $venv @())) { $py = $venv }
elseif (Test-Py "py" @("-3")) { $py = "py"; $pyArgs = @("-3") }
elseif (Test-Py "python" @()) { $py = "python" }
if (-not $py) {
    Write-Host "== Installing Python 3.12 ==" -ForegroundColor Cyan
    winget install --id Python.Python.3.12 -e --source winget --accept-source-agreements --accept-package-agreements
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
    if (Test-Py "py" @("-3")) { $py = "py"; $pyArgs = @("-3") }
    elseif (Test-Py "python" @()) { $py = "python" }
    else { Write-Host "Python installed. Close this window, open a new PowerShell and run the script again." -ForegroundColor Yellow; exit 1 }
}

# 2) esptool
& $py @pyArgs -m esptool version 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "== Installing esptool ==" -ForegroundColor Cyan
    & $py @pyArgs -m pip install --quiet "esptool>=4.7,<5"
    if ($LASTEXITCODE -ne 0) { Write-Host "Could not install esptool (internet?)" -ForegroundColor Red; exit 1 }
}

# 3) find the board (Espressif USB = VID 303A)
$port = $args[0]
if (-not $port) {
    foreach ($d in Get-CimInstance Win32_PnPEntity) {
        if ($d.PNPDeviceID -match "VID_303A" -and $d.Name -match "\((COM\d+)\)") { $port = $Matches[1]; break }
    }
}
if (-not $port) {
    Write-Host "Board not found. Check the USB cable (must be a data cable)." -ForegroundColor Red
    Write-Host "If it still doesn't show up: hold BOOT, press RESET, release BOOT, then run this again." -ForegroundColor Yellow
    exit 1
}
Write-Host "== Board on $port, writing firmware ==" -ForegroundColor Cyan

# 4) write
& $py @pyArgs -m esptool --chip esp32s3 --port $port --baud 921600 --before default_reset --after hard_reset `
    write_flash -z --flash_mode keep --flash_freq keep --flash_size keep `
    0x0     (Join-Path $bin "bootloader.bin") `
    0x8000  (Join-Path $bin "partitions.bin") `
    0xe000  (Join-Path $bin "boot_app0.bin") `
    0x10000 (Join-Path $bin $APP)
if ($LASTEXITCODE -ne 0) {
    Write-Host "Write failed. Close run.bat / Arduino IDE (they hold the port), or: hold BOOT, press RESET, release BOOT, and retry." -ForegroundColor Red
    exit 1
}
Write-Host "== Done! Press RESET on the board if the face doesn't appear. ==" -ForegroundColor Green


