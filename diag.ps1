# Shows the board's start-up log. Usage:  powershell -ExecutionPolicy Bypass -File .\diag.ps1
function Test-Py($cmd, $extra) {
    try { & $cmd @extra --version 2>$null | Out-Null; return $LASTEXITCODE -eq 0 } catch { return $false }
}
$py = $null; $pyArgs = @()
$venv = Join-Path $PSScriptRoot "pc\.venv\Scripts\python.exe"
if (Test-Path $venv) { $py = $venv }
elseif (Test-Py "py" @("-3")) { $py = "py"; $pyArgs = @("-3") }
elseif (Test-Py "python" @()) { $py = "python" }
if (-not $py) { Write-Host "Python not found - run flash.ps1 first." -ForegroundColor Red; exit 1 }

& $py @pyArgs -c "import serial" 2>$null
if ($LASTEXITCODE -ne 0) { & $py @pyArgs -m pip install --quiet pyserial }
& $py @pyArgs (Join-Path $PSScriptRoot "pc\diag.py")
