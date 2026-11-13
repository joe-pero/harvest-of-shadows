# Flashing yard props over a Raspberry Pi

Props live outside: in a tree, behind a fence, inside a sealed box. Carrying each one to a
laptop (or a laptop out to each one) gets old fast, and some can't be unplugged without
taking the display apart.

The fix is a small, cheap **USB bridge**: a Raspberry Pi left in the yard on Wi-Fi. Plug a
prop's ESP32 into the Pi's USB port, and you can flash it, read its serial log and run bench
tests from your desk over SSH.

This guide is what one season of doing that taught me, including the mistakes that cost the
most time.

```mermaid
flowchart LR
    PC["Desk PC<br/>(builds firmware)"] -- "scp + ssh<br/>over Wi-Fi" --> PI["Raspberry Pi<br/>in the yard"]
    PI -- USB --> ESP["Prop's ESP32"]
```

## Set up the Pi

1. **Raspberry Pi OS Lite** (64-bit). No desktop: less to update, less to break.
2. **Key-only SSH.** Add your public key, then set `PasswordAuthentication no` in
   `/etc/ssh/sshd_config`. A box sitting outside should not accept passwords.
3. **Automatic security updates:** `sudo apt install unattended-upgrades`.
4. **Tools:** `sudo apt install esptool python3-serial screen`.
5. **Serial access without sudo:** `sudo usermod -aG dialout $USER`, then log out and back in.
6. **Reach it by name, not address.** Home routers hand out addresses that change. Use the
   Pi's `.local` name (mDNS), or reserve an address for it on your router.

> **Before removing packages on the Pi**, run `sudo apt-get purge --simulate <package>` first.
> On mine, removing Bluetooth (`bluez`) also tried to remove the network manager, which would
> have left the Pi unreachable after the next reboot.

## Flash safely: the five rules

**1. Check the firmware file *on the Pi* before flashing it.**
The Pi once lost power about three minutes after a file copy. The copied firmware was all
zeros (the file system hadn't written it to the card yet). esptool flashed the zeros and still
reported "Hash of data verified", because it checks the chip against the *file*, not the file
against your build. The prop wouldn't boot until it was reflashed.
Always run `sync` after copying, and compare the file's checksum on the Pi with your build's
before you flash. [`tools/bridge_flash.sh`](../tools/bridge_flash.sh) does both.

**2. Close every serial monitor before flashing.**
If anything still has the port open (`screen`, `cat`, a logging script), the flash fails or
hangs. Killing the SSH session on your desk does *not* reliably stop the process on the Pi;
it can keep running there. Check on the Pi itself: `fuser -v /dev/ttyUSB0`.

**3. Opening the serial port reboots most ESP32 dev boards.**
Their USB chip uses two control lines to reset the board, and opening the port toggles them,
even with those lines set low in your code. So never open a serial monitor on a prop that's
in the middle of a live show, and design bench tools so every command carries its own
settings (the board forgets everything on reconnect).

**4. Some Debian builds of esptool need `--no-stub`.**
If you see a `FileNotFoundError` about a stub JSON file, the packaged esptool is missing its
fast "stub" loader. `--no-stub` works, just more slowly. A full flash *read-back* over
`--no-stub` can take around 25 minutes, so to back up a board, rebuild the firmware from the
exact source version instead.

**5. One power source at a time.**
If a prop has its own 5 V supply on the ESP32's VIN pin, unplug that before connecting USB to
the Pi. With both connected, the USB supply can push power backwards into the prop's
converter, and the board goes silent in a way that looks like a dead chip. Flash with the
prop's own power off, keeping the grounds connected.

## A quick serial log

```bash
ssh pi@yard-bridge.local 'stty -F /dev/ttyUSB0 115200 raw -echo && timeout 60 cat /dev/ttyUSB0' > prop.log
```

`timeout` makes sure the read ends on the Pi too (see rule 2).

## Tip: noisy logs mean a noisy supply

On one prop the serial log was clean with the prop's main power off and full of garbage
with it on. The cause was electrical noise from the prop's amplifier wiring, not the firmware.
If a log looks corrupted, try reading it with the prop's own power switched off.

## Cut USB cables: meter before you trust the colors

A prop was killed by a cut USB cable whose two wires were red and white. Red was assumed to
be +5 V. It wasn't. Charge-only cables follow no color standard. Meter the wires before
they touch a board, or better, don't cut cables at all: use an intact cable into the board's
own USB port.

## What about USB-over-IP tools?

I also tried a USB-over-IP product so desktop tools would see the yard's ports as local.
The free tier blocked the setup I needed, and the desktop client never showed its window on
my Linux desktop. Plain SSH and esptool on the Pi turned out simpler and more reliable.
