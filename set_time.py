# Sets Laboo's clock from this computer's time (Tehran). Close anything else using the port first.
# Usage: python set_time.py [COM8]
import sys, time, datetime, serial, serial.tools.list_ports
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

port = sys.argv[1] if len(sys.argv) > 1 else next(
    (p.device for p in serial.tools.list_ports.comports() if p.vid == 0x303A), None)
if not port:
    sys.exit("Board not found")
s = serial.Serial(port, 921600, timeout=0.3)
time.sleep(1.0)
s.reset_input_buffer()
s.write(b"PING\n"); time.sleep(0.3)
print(s.read(400).decode(errors="replace").strip())
# wait for the start of a new second so the clock is exact
t = datetime.datetime.now()
time.sleep(1 - t.microsecond / 1e6)
t = datetime.datetime.now()
s.write(f"TIME {t.year} {t.month} {t.day} {t.hour} {t.minute} {t.second}\n".encode())
time.sleep(0.4)
print(s.read(400).decode(errors="replace").strip())
