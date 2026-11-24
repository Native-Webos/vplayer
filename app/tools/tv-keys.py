# SPDX-License-Identifier: GPL-3.0-or-later
# Runs ON the TV (Python 2.7, root): presses remote keys by writing Linux input events to the IR
# remote's event device, so the app sees them exactly like the real remote. Testing only.
#   python tv-keys.py KEY [KEY...]     KEY: a Linux key code, or sleep:<seconds>
# tools/tv-keys.sh pushes this file and turns key names into codes.
import struct
import sys
import time

DEVICE = "/dev/input/event1"  # "LGE RCU" on an OLED65C1 (webOS 6); see /proc/bus/input/devices
FMT = "llHHi"  # struct input_event on 32-bit ARM: timeval, type, code, value


def main():
    dev = open(DEVICE, "wb", 0)

    def event(kind, code, value):
        now = time.time()
        dev.write(struct.pack(FMT, int(now), int((now % 1) * 1e6), kind, code, value))

    for arg in sys.argv[1:]:
        if arg.startswith("sleep:"):
            time.sleep(float(arg[6:]))
            continue
        code = int(arg)
        event(1, code, 1)  # EV_KEY down
        event(0, 0, 0)     # EV_SYN
        time.sleep(0.08)
        event(1, code, 0)  # EV_KEY up
        event(0, 0, 0)
        time.sleep(0.35)


main()
