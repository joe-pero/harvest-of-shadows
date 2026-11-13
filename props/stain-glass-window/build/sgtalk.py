#!/usr/bin/env python3
"""Talk to the Stain Glass/Front Door ESP32 (bench fw) from the bridge Pi.
usage: sgtalk.py <listen_secs> "cmd1" "cmd2" ...   e.g. sgtalk.py 40 "send db PING" "send db STATUS"
Opening the port reboots the ESP32 (known); we wait for its banner first."""
import serial, sys, time
DEV = "/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0"
p = serial.Serial(DEV, 115200, timeout=0.2)
def pump(sec):
    end = time.time() + sec
    while time.time() < end:
        b = p.read(4096)
        if b: sys.stdout.write(b.decode("ascii", "replace")); sys.stdout.flush()
pump(3)
listen = float(sys.argv[1])
for c in sys.argv[2:]:
    print(f"\n>>> {c}", flush=True)
    p.write((c + "\r\n").encode())
    pump(3)
pump(listen)
