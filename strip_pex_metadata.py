#!/usr/bin/env python3
"""Strip identifying metadata from a Fallout 4 Papyrus .pex file.

The .pex header stores three strings that leak the build machine's identity:
  sourceFileName  (e.g. "Z:\\home\\<user>\\projects\\...\\Foo.psc")
  userName        (the OS user that compiled it)
  machineName     (the computer name)

These show up verbatim in the game's Papyrus logs / stack traces. This rewrites
them to neutral values: sourceFileName -> bare "<Name>.psc", user/machine -> "".
Line-number debug info is left intact, so stack traces stay useful.

Usage: strip_pex_metadata.py FILE.pex [FILE.pex ...]
"""
import struct
import sys

MAGIC = 0xFA57C0DE  # Fallout 4 .pex magic, little-endian


def read_str(buf, off):
    (n,) = struct.unpack_from("<H", buf, off)
    off += 2
    return buf[off:off + n].decode("latin-1"), off + n


def write_str(s):
    b = s.encode("latin-1")
    return struct.pack("<H", len(b)) + b


def scrub(path):
    with open(path, "rb") as f:
        buf = f.read()

    (magic,) = struct.unpack_from("<I", buf, 0)
    if magic != MAGIC:
        raise SystemExit(f"{path}: not a Fallout 4 .pex (magic={magic:#010x})")

    # magic(4) + major(1) + minor(1) + gameID(2) + compileTime(8) = 16 bytes
    off = 16
    src, off = read_str(buf, off)
    user, off = read_str(buf, off)
    machine, off = read_str(buf, off)
    rest = buf[off:]

    new_src = src.replace("\\", "/").rsplit("/", 1)[-1]  # basename only
    new_header = (
        buf[:16]
        + write_str(new_src)
        + write_str("")   # userName
        + write_str("")   # machineName
    )
    with open(path, "wb") as f:
        f.write(new_header + rest)

    print(f"{path}:")
    print(f"  sourceFileName: {src!r} -> {new_src!r}")
    print(f"  userName:       {user!r} -> ''")
    print(f"  machineName:    {machine!r} -> ''")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for p in sys.argv[1:]:
        scrub(p)
