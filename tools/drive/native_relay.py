# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""The process here that stands for a program on a Windows laptop.

    python3 native_relay.py <port>

testbed.py wants a process whose stderr is the program's and which ends when
the program does. On a VirtualBox guest that is the VBoxManage that runs it.
A laptop has no such thing: its program is started by the agent on its
desktop (windows_agent.ps1, `run`), which then calls here -- through the
loopback port native_driver.py's ssh forwards -- and sends what the program
writes. This copies it to stderr and leaves with the program's exit code.

Its own file, and nothing imported that is not in the box: it has to be
listening a moment after it is started, which is when the agent first tries.
"""

import socket
import sys

# What the agent waits for before it believes anybody is here: a forwarded
# port accepts whether or not this is behind it.
HERE = b"\x06"
# What comes before the exit code, which no program here prints.
END = b"\x1e"


def main():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", int(sys.argv[1])))
    srv.listen(1)
    srv.settimeout(30.0)
    try:
        conn, _ = srv.accept()
    except socket.timeout:
        print("the agent never brought the program's output",
              file=sys.stderr)
        return 125
    finally:
        srv.close()
    out = sys.stderr.buffer
    tail = None
    # A connection the laptop's end reset is the stream's end, said: the
    # exit code decides below, which a reset before it leaves at 125.
    try:
        conn.sendall(HERE)
    except OSError as e:
        print("the agent's connection broke: %s" % e, file=sys.stderr)
        return 125
    while True:
        try:
            data = conn.recv(4096)
        except OSError as e:
            print("the agent's connection broke: %s" % e, file=sys.stderr)
            break
        if not data:
            break
        if tail is None and END in data:
            data, tail = data.split(END, 1)
        elif tail is not None:
            tail += data
            continue
        out.write(data)
        out.flush()

    words = (tail or b"").split()
    if len(words) != 2 or words[0] != b"exit":
        print("the program's output stopped without its exit code",
              file=sys.stderr)
        return 125
    code = int(words[1])
    if code:
        # Windows' codes are thirty-two bits and a status here is eight, so
        # the whole of it is said before it is cut down.
        print("the program left with 0x%08X" % (code & 0xffffffff),
              file=sys.stderr)
    return code & 0xff or (1 if code else 0)


if __name__ == "__main__":
    sys.exit(main())
