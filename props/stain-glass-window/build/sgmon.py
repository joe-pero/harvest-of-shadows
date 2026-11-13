#!/usr/bin/env python3
"""Long-running monitor for the Stain Glass/Front Door ESP32, run ON the bridge Pi.

Holds the serial port open (opening it reboots the ESP32, so open once and keep it), writes every
"["-tagged line to ~/sg_monitor.log with a wall-clock time, and drops the known line-tail repeat flood
(docs/hardware-gotchas.md: keep whole "["-prefixed lines, de-duplicate).
Commands: append a line to ~/sg_cmd (e.g. `echo mon >> ~/sg_cmd`, `echo "send db PING" >> ~/sg_cmd`);
the file is read + emptied every second. sgtalk.py must NOT be used while this runs (shared port).
Start: nohup python3 ~/sgmon.py >/dev/null 2>&1 &     Stop: kill $(cat ~/sgmon.pid)
"""
import os, serial, time

DEV = "/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0"
HOME = os.path.expanduser("~")
LOG, CMD, PID = (os.path.join(HOME, f) for f in ("sg_monitor.log", "sg_cmd", "sgmon.pid"))

with open(PID, "w") as f:
    f.write(str(os.getpid()))
log = open(LOG, "a", buffering=1)


def out(text):
    log.write(time.strftime("%Y-%m-%d %H:%M:%S ") + text + "\n")


port = serial.Serial(DEV, 115200, timeout=0.2)
out("[sgmon] port opened (ESP32 reboots now)")
buf, last, last_cmd_check = b"", None, 0.0
while True:
    buf += port.read(4096)
    while b"\n" in buf:
        raw, buf = buf.split(b"\n", 1)
        line = raw.decode("ascii", "replace").strip()
        i = line.find("[")
        if i < 0:
            continue
        line = line[i:]
        if line != last:
            out(line)
        last = line
    if len(buf) > 8192:          # flood without newlines
        buf = b""
    now = time.time()
    if now - last_cmd_check >= 1.0:
        last_cmd_check = now
        try:
            with open(CMD, "r+") as f:
                cmds = f.read().splitlines()
                f.seek(0); f.truncate()
        except FileNotFoundError:
            cmds = []
        for c in cmds:
            if c.strip():
                out(f"[sgmon] >>> {c.strip()}")
                port.write((c.strip() + "\r\n").encode())
                last = None
