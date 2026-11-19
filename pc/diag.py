"""Asks the board for its start-up report and shows what it prints."""
import sys
import time

import serial
from serial.tools import list_ports

SECONDS = 20


def find():
    for p in list_ports.comports():
        if p.vid == 0x303A:
            return p.device
    return None


print(f">>> Asking the board for its report ({SECONDS} s). Stay quiet for 5 seconds, then talk loudly / clap near the board.\n", flush=True)
end = time.time() + SECONDS
last_ping = 0
while time.time() < end:
    port = find()
    if not port:
        time.sleep(0.2)
        continue
    try:
        with serial.Serial(port, 115200, timeout=0.2) as s:
            s.write(b"INFO\n")
            while time.time() < end:
                if time.time() - last_ping > 3:
                    s.write(b"PING\n")
                    last_ping = time.time()
                d = s.read(4096)
                if d:
                    txt = "".join(chr(b) if (32 <= b < 127 or b in (10,)) else "" for b in d)
                    sys.stdout.write(txt)
                    sys.stdout.flush()
    except Exception as e:
        print(f"\n[port closed: {e.__class__.__name__}, reconnecting]", flush=True)
        time.sleep(0.3)
print("\n>>> done")
