#!/usr/bin/env python3
"""Sets an input of the Colibri emulator and follows an output, over its control socket.

The control protocol (emulators/specifications/01-control-protocol.md) is newline-delimited
JSON: every message is an object with one member. This script needs nothing but Python 3.

    examples/follow.py [--socket PATH] [--input KEY] [--output KEY] [--seconds N]

With the defaults it gives slot1.ai.1 (an AIV in slot 1) a sine between 0 and 10 V over 10
seconds and prints every value of slot3.dq.1 (an SSR in slot 3), which a hakkas process may
drive from that input, e.g. a compare/Greater against 5.
"""

import argparse
import json
import os
import socket
import sys
import tempfile
import time


def default_socket():
    """The emulator's default --control-socket (its spec, section 3.1)."""
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    base = (os.path.join(runtime, "colibri-emulator") if runtime
            else os.path.join(tempfile.gettempdir(), f"colibri-emulator-{os.getuid()}"))
    return os.path.join(base, "control.sock")


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--socket", default=default_socket(), help="the emulator's --control-socket")
    p.add_argument("--input", default="slot1.ai.1", help="the input to set")
    p.add_argument("--output", default="slot3.dq.1", help="the output to follow")
    p.add_argument("--seconds", type=float, default=30, help="how long to follow it")
    args = p.parse_args()

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(args.socket)
    f = s.makefile("rw", encoding="utf-8", newline="\n")

    def send(message):
        f.write(json.dumps(message) + "\n")
        f.flush()

    send({"set": {"id": 1, "key": args.input + ".source",
                  "value": {"sine": {"offset": 5, "amplitude": 5, "period": 10}}}})
    s.settimeout(0.5)
    end = time.monotonic() + args.seconds
    last = None
    while time.monotonic() < end:
        try:
            line = f.readline()
        except (socket.timeout, TimeoutError):
            continue
        if not line:
            sys.exit("the emulator closed the connection")
        message = json.loads(line)
        name, body = next(iter(message.items()))
        if name == "error":
            sys.exit(f"refused: {body['message']}")
        if name == "ok" and body.get("id") == 1:
            print(f"{args.input} follows a sine, 0-10 over 10 s")
        elif name == "link":
            print(f"hakkas: {body['state']}")
        elif name == "state" and args.output in body["values"]:
            value = body["values"][args.output]
            if value != last:
                print(f"{time.strftime('%H:%M:%S')} {args.output} = {value}")
                last = value


if __name__ == "__main__":
    main()
