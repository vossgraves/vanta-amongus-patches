#!/usr/bin/env python3
# language: python, file: tools/dump-offsets.py
"""Regenerate native/src/offsets.h from a dump.cs.

    python3 tools/dump-offsets.py dump.cs --version-code 7616 --version-name 2026.11.3 \
        > native/src/offsets.h

Input is Il2CppDumper's dump.cs:

    Il2CppDumper libil2cpp.so global-metadata.dat outdir

The script resolves each symbol by class + method signature, never by line number, and
fails loudly when a symbol disappears so a game update cannot silently emit a zero RVA.
"""

from __future__ import annotations

import argparse
import re
import sys

# symbol key -> (class, member kind, member name, member signature fragment)
METHODS = {
    "PlayerControl_FixedUpdate": ("PlayerControl", "method", "FixedUpdate", "void FixedUpdate()"),
    "PlayerControl_SetKillTimer": ("PlayerControl", "method", "SetKillTimer", "void SetKillTimer("),
    "PlayerControl_CmdCheckMurder": ("PlayerControl", "method", "CmdCheckMurder", "void CmdCheckMurder("),
    "PlayerControl_RpcMurderPlayer": ("PlayerControl", "method", "RpcMurderPlayer", "void RpcMurderPlayer("),
    "PlayerControl_RpcCompleteTask": ("PlayerControl", "method", "RpcCompleteTask", "void RpcCompleteTask("),
    "PlayerPhysics_LateUpdate": ("PlayerPhysics", "method", "LateUpdate", "void LateUpdate()"),
    "Vent_CanUse": ("Vent", "method", "CanUse", "float CanUse("),
    "PlayerPurchasesData_GetPurchase": ("PlayerPurchasesData", "method", "GetPurchase", "bool GetPurchase("),
    "EOSManager_IsFreechatAllowed": ("EOSManager", "method", "IsFreechatAllowed", "bool IsFreechatAllowed()"),
    "EOSManager_IsFriendsListAllowed": ("EOSManager", "method", "IsFriendsListAllowed", "bool IsFriendsListAllowed()"),
    "EOSManager_IsAllowedOnline": ("EOSManager", "method", "IsAllowedOnline", "void IsAllowedOnline("),
    "EOSManager_IsMinorOrWaiting": ("EOSManager", "method", "IsMinorOrWaiting", "bool IsMinorOrWaiting("),
    "FullAccount_CanSetCustomName": ("FullAccount", "method", "CanSetCustomName", "void CanSetCustomName("),
    "AccountManager_CanPlayOnline": ("AccountManager", "method", "CanPlayOnline", "bool CanPlayOnline()"),
    "GameManager_CheckTaskCompletion": ("GameManager", "method", "CheckTaskCompletion", "bool CheckTaskCompletion()"),
    "InnerNetObject_get_AmOwner": ("InnerNetObject", "method", "get_AmOwner", "bool get_AmOwner()"),
}

FIELDS = {
    "PlayerControl_MyPhysics": ("PlayerControl", "MyPhysics"),
    "PlayerControl_myTasks": ("PlayerControl", "myTasks"),
    "PlayerPhysics_Speed": ("PlayerPhysics", "Speed"),
    "PlayerPhysics_GhostSpeed": ("PlayerPhysics", "GhostSpeed"),
    "PlayerTask_Id": ("PlayerTask", "Id"),
}

CLASS_RE = re.compile(r"^(?:public|internal) (?:sealed |abstract )?(?:class|struct) ([\w.]+)")
RVA_RE = re.compile(r"// RVA: (0x[0-9A-Fa-f]+)")
FIELD_RE = re.compile(r"// 0x([0-9A-Fa-f]+)\s*$")


def split_classes(src: str):
    """Yield (class_name, body) for each top-level type declaration."""
    lines = src.splitlines()
    current = None
    start = None
    depth = 0
    for i, line in enumerate(lines):
        if current is None:
            m = CLASS_RE.match(line)
            if m:
                current = m.group(1)
                start = i
                depth = 0
        if current is not None:
            depth += line.count("{") - line.count("}")
            if depth == 0 and i > start and "}" in line:
                yield current, "\n".join(lines[start:i + 1])
                current = None


def resolve(src: str):
    methods: dict[str, int] = {}
    fields: dict[str, int] = {}

    bodies = {}
    for name, body in split_classes(src):
        bodies.setdefault(name, body)

    for key, (cls, _kind, name, sig) in METHODS.items():
        body = bodies.get(cls)
        if body is None:
            raise SystemExit(f"class {cls} not in dump")
        lines = body.splitlines()
        found = None
        for i in range(len(lines) - 1):
            nxt = lines[i + 1]
            if "RVA:" in lines[i] and sig in nxt:
                found = int(RVA_RE.search(lines[i]).group(1), 16)
                break
        if found is None:
            raise SystemExit(f"method {cls}.{name} not in dump")
        methods[key] = found

    for key, (cls, name) in FIELDS.items():
        body = bodies.get(cls)
        if body is None:
            raise SystemExit(f"class {cls} not in dump")
        pattern = re.compile(r"// 0x([0-9A-Fa-f]+)\s*$")
        found = None
        for line in body.splitlines():
            # "public float Speed; // 0x50" or "private uint <Id>k__BackingField; // 0x24"
            m = re.search(r"// 0x([0-9A-Fa-f]+)\s*$", line)
            if not m or "RVA" in line:
                continue
            decl = line.split("//")[0].strip().rstrip(";")
            ident = decl.split()[-1] if decl.split() else ""
            if ident == name or ident == f"<{name}>k__BackingField":
                found = int(m.group(1), 16)
                break
        if found is None:
            raise SystemExit(f"field {cls}.{name} not in dump")
        fields[key] = found

    return methods, fields


HEADER = """// language: C++, file: native/src/offsets.h
// Generated by tools/dump-offsets.py — do not edit by hand.
//
// {package} {version_name} (versionCode {version_code}), metadata {metadata}
// Regenerate after every game update, then rebuild libvanta.so.

#pragma once
#include <cstdint>

namespace vanta::off {{

constexpr int kVersionCode = {version_code};
constexpr char kVersionName[] = "{version_name}";

// Method entry points.
{methods}
// Field offsets (managed object layout).
{fields}
// Il2Cpp List<T> instance layout on arm64: object header is 16 bytes.
constexpr uint32_t List_items = 0x10;
constexpr uint32_t List_size  = 0x18;
constexpr uint32_t Array_data = 0x20;

}}  // namespace vanta::off
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--version-code", required=True, type=int)
    ap.add_argument("--version-name", required=True)
    ap.add_argument("--package", default="com.innersloth.spacemafia")
    ap.add_argument("--metadata", default="31")
    args = ap.parse_args()

    src = open(args.dump, encoding="utf-8", errors="ignore").read()
    methods, fields = resolve(src)

    lines = []
    for key, value in methods.items():
        lines.append(f"constexpr uint32_t {key} = 0x{value:X};")
    field_lines = []
    for key, value in fields.items():
        field_lines.append(f"constexpr uint32_t {key} = 0x{value:X};")

    sys.stdout.write(HEADER.format(
        package=args.package,
        version_name=args.version_name,
        version_code=args.version_code,
        metadata=args.metadata,
        methods="\n".join(lines) + "\n",
        fields="\n".join(field_lines) + "\n",
    ))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
