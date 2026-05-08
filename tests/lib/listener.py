#!/usr/bin/env python3
"""Test-only UDS listener for FsEventBridge.

Connects to the given socket, reads NDJSON events, prints each event line to
stdout, and exits when either:
  - the requested number of events has been received, or
  - the timeout window elapses.

Designed to be deterministic for shell-based assertions.
"""

import argparse
import os
import socket
import sys
import time


def wait_for_socket(path: str, deadline: float) -> None:
    while not os.path.exists(path):
        if time.monotonic() > deadline:
            print(f"listener: socket not appeared: {path}", file=sys.stderr)
            sys.exit(2)
        time.sleep(0.05)


def connect(path: str, deadline: float) -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    while True:
        try:
            s.connect(path)
            return s
        except (FileNotFoundError, ConnectionRefusedError):
            if time.monotonic() > deadline:
                print(f"listener: failed to connect: {path}", file=sys.stderr)
                sys.exit(2)
            time.sleep(0.05)


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--socket", required=True)
    p.add_argument("--timeout", type=float, default=5.0,
                   help="overall read window (seconds)")
    p.add_argument("--count", type=int, default=0,
                   help="exit after N events; 0 means read until timeout")
    p.add_argument("--connect-timeout", type=float, default=5.0)
    args = p.parse_args()

    connect_deadline = time.monotonic() + args.connect_timeout
    wait_for_socket(args.socket, connect_deadline)
    s = connect(args.socket, connect_deadline)

    buf = b""
    received = 0
    deadline = time.monotonic() + args.timeout

    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            s.settimeout(remaining)
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, _, buf = buf.partition(b"\n")
                line = line.strip()
                if not line:
                    continue
                sys.stdout.write(line.decode("utf-8", errors="replace") + "\n")
                sys.stdout.flush()
                received += 1
                if args.count and received >= args.count:
                    return
    finally:
        s.close()


if __name__ == "__main__":
    main()
