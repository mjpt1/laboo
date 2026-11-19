# Writes the Lumo-style test face to the board.  To go back, use flash.bat in
# archive\faces_2026-10-27\v2_current (current face) or ...\v1 (first face).
$APP = "laboo_face_lumo.bin"
$bin = $PSScriptRoot
foreach ($f in "bootloader.bin", "partitions.bin", "boot_app0.bin", $APP) {
    if (-not (Test-Path (Join-Path $bin $f))) { Write-Host "Missing $f" -ForegroundColor Red; exit 1 }
}
function Test-Py($cmd, $extra) { try { & $cmd @extra --version 2>$null | Out-Null; return $LASTEXITCODE -eq 0 } catch { return $false } }
$py = $null; $pyArgs = @()
$venv = Join-Path $PSScriptRoot "..\..\..\pc\.venv\Scripts\python.exe"
if ((Test-Path $venv) -and (Test-Py $venv @())) { $py = $venv } else { try { & py -3 --version 2>$null | Out-Null; if ($LASTEXITCODE -eq 0) { $py = "py"; $pyArgs = @("-3") } } catch {} }
if (-not $py) { $py = "python" }
& $py @pyArgs -m esptool version 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { & $py @pyArgs -m pip install --quiet "esptool>=4.7,<5" }
$port = $args[0]
if (-not $port) {
    foreach ($d in Get-CimInstance Win32_PnPEntity) {
        if ($d.PNPDeviceID -match "VID_303A" -and $d.Name -match "\((COM\d+)\)") { $port = $Matches[1]; break }
    }
}
if (-not $port) { Write-Host "Board not found. Check the USB cable (must be a data cable)." -ForegroundColor Red; exit 1 }
Write-Host "== Board on $port, writing Lumo-style face ==" -ForegroundColor Cyan
& $py @pyArgs -m esptool --chip esp32s3 --port $port --baud 921600 --before default_reset --after hard_reset `
    write_flash -z --flash_mode keep --flash_freq keep --flash_size keep `
    0x0 (Join-Path $bin "bootloader.bin") 0x8000 (Join-Path $bin "partitions.bin") `
    0xe000 (Join-Path $bin "boot_app0.bin") 0x10000 (Join-Path $bin $APP)
if ($LASTEXITCODE -ne 0) { Write-Host "Write failed. Close run.bat / Arduino IDE (they hold the port) and retry." -ForegroundColor Red; exit 1 }
Write-Host "== Done ==" -ForegroundColor Green

