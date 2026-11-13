#!/usr/bin/env python3
"""Serial monitor for the A1S boards that does NOT reset them (2026-09-25).

Run ON the yard-bridge-pi Pi. pyserial's default open() asserts DTR/RTS, which
pulses EN on these boards: the ESP32 reboots and logs rst:0x1 POWERON_RESET
(an EN reset reports as POWERON), so a monitor opened the normal way reboots
the board you're trying to watch -- and any cue sent during that reboot is lost.
Setting dtr/rts BEFORE open() avoids it.

  scp tools/serial_monitor.py pi@<pi>:/tmp/ && \
  ssh pi@<pi> 'nohup python3 /tmp/serial_monitor.py 7200 > /tmp/serial.log 2>&1 & echo $! > /tmp/serial_monitor.pid'
Stop it by PID (never `pkill -f serial_monitor` over ssh: it matches the ssh
command itself), and stop it before esptool write_flash (port busy).
"""
import sys, time, serial
port = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyUSB0"
s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.2
s.dtr = False; s.rts = False; s.open()
end = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 60)
while time.time() < end:
    b = s.read(4096)
    if b:
        sys.stdout.write(b.decode("utf-8", "replace")); sys.stdout.flush()
