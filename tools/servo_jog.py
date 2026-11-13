#!/usr/bin/env python3
"""Jog one crow servo by keyboard and mark its limits (shoulder crows, firmware with POS).

  python3 tools/servo_jog.py            # head (GPIO 13)
  python3 tools/servo_jog.py 14         # tilt

Keys:  a / d  = 1 deg left / right     A / D (shift) = 5 deg
       c      = centre (90)            1 = mark LOW limit    2 = mark HIGH limit
       x      = servos off (limp)      q = quit (prints + saves the marks)

Uses the board's POS command (holds the other servo still, pauses idle moves 10 min).
Marks are saved to /tmp/servo_jog_marks.txt. Only one program can hold the port: close
any serial monitor first.
"""
import sys, termios, tty, time, select, serial

PORT = "/dev/ttyUSB0"
pin = int(sys.argv[1]) if len(sys.argv) > 1 else 13
other = 14 if pin == 13 else 13
name = "HEAD" if pin == 13 else "TILT"

s = serial.Serial()
s.port, s.baudrate, s.timeout = PORT, 115200, 0
s.dtr = s.rts = False
s.open()
print("Connecting (the board may reboot)...")
print("HEAD ONLY (tilt held at 90).  <- / -> or a/d = 1 deg,  A/D = 5 deg,  r = flip left/right,\n"
      "c = centre,  1 = mark LOW,  2 = mark HIGH,  x = off,  q = quit")
time.sleep(3.5)


def send(cmd):
    s.write((cmd + "\n").encode())
    time.sleep(0.05)
    s.reset_input_buffer()  # ignore replies (this host's serial echo floods them)


send("MUTE")
send("SERVOS ON")
send(f"POS {other} 90")
angle = 90
send(f"POS {pin} {angle}")
low = high = None
flip = 1  # r flips which key turns which way

old = termios.tcgetattr(sys.stdin)
tty.setcbreak(sys.stdin.fileno())


def show():
    lo = "-" if low is None else low
    hi = "-" if high is None else high
    sys.stdout.write(f"\r{name} {angle:3d} deg  LOW={lo} HIGH={hi}   ")  # short: must not wrap
    sys.stdout.flush()


try:
    show()
    while True:
        if not select.select([sys.stdin], [], [], 0.2)[0]:
            continue
        k = sys.stdin.read(1)
        if k == "\x1b":  # arrow keys: ESC [ C (right) / ESC [ D (left)
            seq = sys.stdin.read(2)
            k = {"[C": "d", "[D": "a"}.get(seq, "")
        step = {"a": -1, "d": 1, "A": -5, "D": 5}.get(k)
        if step:
            angle = max(0, min(180, angle + step * flip))
            send(f"POS {pin} {angle}")
        elif k == "c":
            angle = 90
            send(f"POS {pin} {angle}")
        elif k == "r":
            flip = -flip
        elif k == "1":
            low = angle
        elif k == "2":
            high = angle
        elif k == "x":
            send("SERVOS OFF")
            sys.stdout.write("\n  servos OFF (any move key turns them back on)\n")
            sys.stdin.read(1)
            send("SERVOS ON")
            send(f"POS {other} 90")
            send(f"POS {pin} {angle}")
        elif k == "q":
            break
        show()
finally:
    termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old)
    send(f"POS {pin} 90")
    time.sleep(0.5)
    send("SERVOS OFF")
    s.close()
    msg = f"{name} GPIO{pin}: LOW={low} HIGH={high} (centre 90), {time.strftime('%Y-%m-%d %H:%M')}"
    with open("/tmp/servo_jog_marks.txt", "a") as f:
        f.write(msg + "\n")
    print("\n" + msg + "\nSaved to /tmp/servo_jog_marks.txt. Servo back at 90, servos off.")
