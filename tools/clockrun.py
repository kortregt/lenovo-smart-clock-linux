#!/usr/bin/env python3
"""Run shell commands on the clock over its USB CDC-ACM serial shell and print the output.

Usage:
    uv run --no-project --with pyserial python tools/clockrun.py 'cmd1' ['cmd2' ...]
    CLOCK_TTY=/dev/cu.usbmodemXXXX ... (default: first /dev/cu.usbmodem*)

Each command's output is delimited with a unique marker, so long outputs (dmesg) are read
completely instead of guessing a timeout.
"""
import glob
import os
import sys
import time
import uuid

import serial


def run(port, cmd, timeout=60):
    tag = uuid.uuid4().hex[:8]
    mark = "__END_" + tag
    # Quoted split so only the output (not an echoed command line) contains the marker.
    port.write(f'{cmd}; echo "__E""ND_{tag}"\n'.encode())
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline and mark.encode() not in buf:
        buf += port.read(65536)
    text = buf.decode("utf-8", "replace").replace("\r", "")
    body = text.split(mark, 1)[0]
    if f'"__E""ND_{tag}"' in body:  # echo still on: drop the echoed command line
        body = body.split(f'"__E""ND_{tag}"', 1)[1].split("\n", 1)[-1]
    return body


def main():
    tty = os.environ.get("CLOCK_TTY") or (sorted(glob.glob("/dev/cu.usbmodem*")) or [None])[0]
    if not tty:
        sys.exit("no /dev/cu.usbmodem* found: is the clock booted into Linux and plugged in?")
    port = serial.Serial(tty, 115200, timeout=0.2)
    port.write(b"stty -echo 2>/dev/null; PS1=''\n")
    time.sleep(0.5)
    port.read(65536)
    try:
        for cmd in sys.argv[1:]:
            sys.stdout.write(run(port, cmd))
    finally:
        # The serial shell is shared with interactive `screen` sessions: restore echo/prompt.
        port.write(b"stty echo 2>/dev/null; PS1='\\h:\\w\\$ '\n")
    sys.stdout.flush()


if __name__ == "__main__":
    main()
