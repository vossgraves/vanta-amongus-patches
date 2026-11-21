#!/usr/bin/env python3
# language: python, file: tools/dexscan.py
"""Minimal DEX reader: list classes with their superclass and method names.

    python3 tools/dexscan.py classes.dex --class app.vanta --json

Used to verify patch targets without jadx: activity onCreate presence, Play Services
classes, and the method set a patch will rewrite.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from typing import Iterator

NO_INDEX = 0xFFFFFFFF


class Dex:
    def __init__(self, data: bytes):
        self.d = data
        (self.string_ids_size, self.string_ids_off,
         self.type_ids_size, self.type_ids_off,
         self.proto_ids_size, self.proto_ids_off,
         self.field_ids_size, self.field_ids_off,
         self.method_ids_size, self.method_ids_off,
         self.class_defs_size, self.class_defs_off) = struct.unpack_from("<12I", data, 0x38)

    # -- tables ---------------------------------------------------------------

    def u32(self, off: int) -> int:
        return struct.unpack_from("<I", self.d, off)[0]

    def uleb(self, off: int) -> tuple[int, int]:
        result = 0
        shift = 0
        while True:
            b = self.d[off]
            off += 1
            result |= (b & 0x7F) << shift
            if not b & 0x80:
                return result, off
            shift += 7

    def string(self, idx: int) -> str:
        off = self.u32(self.string_ids_off + idx * 4)
        _, off = self.uleb(off)  # utf16 length
        end = self.d.index(b"\x00", off)
        return self.d[off:end].decode("utf-8", errors="replace")

    def type_name(self, idx: int) -> str:
        return self.string(self.u32(self.type_ids_off + idx * 4))

    def proto(self, idx: int) -> str:
        _short, ret_idx, params_off = struct.unpack_from(
            "<III", self.d, self.proto_ids_off + idx * 12)
        ret = self.type_name(ret_idx)
        params = ""
        if params_off:
            size = self.u32(params_off)
            params = "".join(
                self.type_name(struct.unpack_from("<H", self.d, params_off + 4 + i * 2)[0]) for i in range(size))
        return f"({params}){ret}"

    def method(self, idx: int) -> tuple[str, str, str]:
        class_idx, proto_idx, name_idx = struct.unpack_from(
            "<HHI", self.d, self.method_ids_off + idx * 8)
        return (self.type_name(class_idx), self.string(name_idx), self.proto(proto_idx))

    # -- classes --------------------------------------------------------------

    def classes(self) -> Iterator[dict]:
        for i in range(self.class_defs_size):
            base = self.class_defs_off + i * 32
            class_idx, _flags, super_idx, _ifs, _src, _ann, data_off, _sv = struct.unpack_from(
                "<8I", self.d, base)
            name = self.type_name(class_idx)
            super_name = None if super_idx == NO_INDEX else self.type_name(super_idx)
            methods = []
            if data_off:
                methods = list(self.class_methods(data_off))
            yield {"class": name, "super": super_name, "methods": methods}

    def class_methods(self, off: int):
        sf, off = self.uleb(off)
        inf, off = self.uleb(off)
        dm, off = self.uleb(off)
        vm, off = self.uleb(off)
        for _ in range(sf + inf):
            _, off = self.uleb(off)
            _, off = self.uleb(off)
        for count in (dm, vm):
            idx = 0
            for _ in range(count):
                diff, off = self.uleb(off)
                idx += diff
                _flags, off = self.uleb(off)
                _code, off = self.uleb(off)
                _klass, name, proto = self.method(idx)
                yield f"{name}{proto}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dex")
    ap.add_argument("--class", dest="cls", default=None,
                    help="match descriptor (substring)")
    ap.add_argument("--with-methods", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    dex = Dex(open(args.dex, "rb").read())
    rows = []
    for entry in dex.classes():
        if args.cls and args.cls not in entry["class"]:
            continue
        row = {"class": entry["class"], "super": entry["super"]}
        if args.with_methods:
            row["methods"] = entry["methods"]
        rows.append(row)

    if args.json:
        json.dump(rows, sys.stdout, indent=1)
    else:
        for row in rows:
            print(f"{row['class']}  extends {row['super']}")
            for name in row.get("methods", []):
                print(f"    {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
