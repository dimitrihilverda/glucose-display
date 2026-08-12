"""Watchdog-reset the board via esptool and log its USB-CDC serial output.

Usage:
  python capture_serial.py <esptool.exe|SKIP> <outfile> [seconds] [port]

'SKIP' skips the reset (attach-only). The board's native USB-Serial/JTAG does
NOT respond to esptool's classic RTS reset -- the watchdog reset is the one
that works. The CDC port re-enumerates after reset, so opening is retried.
Requires pyserial.
"""
import subprocess, sys, time
import serial

ESPTOOL = sys.argv[1]
OUT = sys.argv[2]
SECONDS = float(sys.argv[3]) if len(sys.argv) > 3 else 90.0
PORT = sys.argv[4] if len(sys.argv) > 4 else "COM5"

if ESPTOOL != "SKIP":
    r = subprocess.run([ESPTOOL, "--port", PORT, "--before", "default-reset",
                        "--after", "watchdog-reset", "chip-id"],
                       capture_output=True, timeout=60, text=True)
    print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip()[-200:])

deadline = time.time() + SECONDS
ser = None
with open(OUT, "wb") as f:
    while time.time() < deadline:
        if ser is None:
            try:
                ser = serial.Serial(PORT, 115200, timeout=0.5)
            except serial.SerialException:
                time.sleep(0.2)
                continue
        try:
            data = ser.read(4096)
        except serial.SerialException:   # port dropped (re-enumeration)
            ser = None
            continue
        if data:
            f.write(data)
            f.flush()
print("capture done")
