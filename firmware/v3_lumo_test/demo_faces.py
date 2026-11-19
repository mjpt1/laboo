# Shows every Lumo-style face on Laboo, one after another (5 s each).
# Close run.bat first (it holds the port). Usage:  python demo_faces.py [COM8]
import sys, time, serial, serial.tools.list_ports
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

port = sys.argv[1] if len(sys.argv) > 1 else next(
    (p.device for p in serial.tools.list_ports.comports() if p.vid == 0x303A), None)
if not port:
    sys.exit("Board not found")
s = serial.Serial(port, 921600, timeout=0.2)
time.sleep(0.5)

def send(cmd):
    s.write((cmd + "\n").encode())

def events(wait=0.6):
    out, end = b"", time.time() + wait
    while time.time() < end:
        out += s.read(4096)
    found, i = [], 0
    while True:
        i = out.find(b"\xaa\x55", i)
        if i < 0 or i + 5 > len(out):
            break
        n = out[i + 3] | (out[i + 4] << 8)
        if out[i + 2] == ord("E") and i + 5 + n <= len(out):
            found.append(out[i + 5:i + 5 + n].decode(errors="replace"))
        i += 5 + n + 1
    return found

send("FACE")
send("PING")
print("board says:", events())
if "--check" in sys.argv:
    sys.exit()

steps = [
    ("E neutral", "عادی"), ("E happy", "خوشحال"), ("E sad", "غمگین"), ("E angry", "عصبانی"),
    ("E surprised", "متعجب"), ("E love", "عاشق"), ("E confused", "گیج"), ("E sleepy", "خواب‌آلود"),
    ("E scared", "ترسیده"), ("E neutral", "عادی"),
    ("S listening", "داره گوش می‌ده"), ("S thinking", "داره فکر می‌کنه"), ("S speaking", "داره حرف می‌زنه"),
    ("S sleeping", "خواب"), ("S idle", "عادی"),
]
for cmd, name in steps:
    print(f"{cmd:<14} {name}")
    send(cmd)
    if cmd == "S speaking":
        for k in range(50):
            send(f"M {(k * 37) % 100}")
            time.sleep(0.1)
    else:
        time.sleep(5)
print("done")
