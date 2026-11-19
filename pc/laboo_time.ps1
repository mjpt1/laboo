# Sets Laboo's clock whenever the board is plugged into this PC (the board has no clock battery).
# Runs hidden from the Startup folder; watches for an Espressif USB serial port (VID 303A).
$sent = @{}
while ($true) {
    $ports = @(Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE '%VID_303A%'" -ErrorAction SilentlyContinue |
        ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $matches[1] } })
    foreach ($c in @($sent.Keys)) { if ($ports -notcontains $c) { $sent.Remove($c) } }   # unplugged: send again next time
    foreach ($c in $ports) {
        if ($sent.ContainsKey($c)) { continue }
        Start-Sleep 8                                   # let it boot and finish the intro
        try {
            $p = New-Object System.IO.Ports.SerialPort $c, 921600
            $p.Open()
            $t = Get-Date
            $p.WriteLine("TIME $($t.Year) $($t.Month) $($t.Day) $($t.Hour) $($t.Minute) $($t.Second)")
            Start-Sleep -Milliseconds 300
            $p.Close()
            $sent[$c] = $true
        } catch { }                                     # port busy (e.g. flashing): try again next round
    }
    Start-Sleep 5
}
