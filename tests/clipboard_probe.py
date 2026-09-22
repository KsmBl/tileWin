#!/usr/bin/env python3
"""Speaks the tilewin-clipboard socket protocol, so a shell test can check it.

  clipboard_probe.py list              one line per entry: <id> <image> <pinned> <preview>
  clipboard_probe.py thumb <id>        prints "<width> <height>" or "none"
  clipboard_probe.py send <command>    sends a command line, e.g. "clear" or "pin 3"
  clipboard_probe.py copy <text>       asks it to put that text on the clipboard
  clipboard_probe.py png <path> <w> <h>   writes a PNG, so no drawing program is needed
"""
import os
import socket
import struct
import sys
import zlib


def socket_path():
    runtime = os.environ["XDG_RUNTIME_DIR"]
    display = os.environ.get("WAYLAND_DISPLAY", "wayland")
    if "/" in display:
        display = "wayland"
    return os.path.join(runtime, "tilewin-clipboard-%s.sock" % display)


def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(socket_path())
    return s


def read_exactly(s, n):
    out = b""
    while len(out) < n:
        chunk = s.recv(n - len(out))
        if not chunk:
            raise EOFError("the clipboard program closed the connection")
        out += chunk
    return out


def read_line(s):
    out = b""
    while not out.endswith(b"\n"):
        ch = s.recv(1)
        if not ch:
            raise EOFError("the clipboard program closed the connection")
        out += ch
    return out[:-1].decode()


def do_list():
    s = connect()
    s.sendall(b"list\n")
    count = int(read_line(s))
    for _ in range(count):
        ident, image, pinned, length = read_line(s).split()
        preview = read_exactly(s, int(length)).decode("utf-8", "replace")
        print("%s %s %s %s" % (ident, image, pinned, preview.replace("\n", "\\n")))
    s.close()


def do_thumb(ident):
    s = connect()
    s.sendall(("thumb %s\n" % ident).encode())
    width, height, stride, length = (int(v) for v in read_line(s).split())
    if width == 0:
        print("none")
    else:
        read_exactly(s, length)
        print("%d %d" % (width, height))
    s.close()


def write_png(path, width, height, noisy=False):
    """A noisy picture barely compresses, so its bytes are worth measuring."""
    rows = b""
    source = os.urandom(width * 3 * height) if noisy else None
    for y in range(height):
        if noisy:
            row = source[y * width * 3:(y + 1) * width * 3]
        else:
            row = bytes([(x * 7 + y * 5) % 256 for x in range(width * 3)])
        rows += b"\x00" + row

    def chunk(kind, body):
        data = kind + body
        return struct.pack(">I", len(body)) + data + struct.pack(">I", zlib.crc32(data))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
           chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    what = sys.argv[1]
    if what == "list":
        do_list()
    elif what == "thumb":
        do_thumb(sys.argv[2])
    elif what == "send":
        s = connect()
        s.sendall((" ".join(sys.argv[2:]) + "\n").encode())
        s.close()
    elif what == "copy":
        text = " ".join(sys.argv[2:]).encode()
        s = connect()
        s.sendall(("copy %d\n" % len(text)).encode() + text)
        s.close()
    elif what == "png":
        write_png(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]),
                  len(sys.argv) > 5 and sys.argv[5] == "noisy")
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
