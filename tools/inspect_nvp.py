#!/usr/bin/env python3
"""Offline regression inspector for NvPresent64.dll. Uses only Python stdlib."""

from __future__ import annotations

import argparse
import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Section:
    name: str
    va: int
    virtual_size: int
    raw_offset: int
    raw_size: int
    characteristics: int


class PE:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        self.pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[self.pe:self.pe + 4] != b"PE\0\0":
            raise ValueError("not a PE image")
        count = struct.unpack_from("<H", self.data, self.pe + 6)[0]
        optional_size = struct.unpack_from("<H", self.data, self.pe + 20)[0]
        self.optional = self.pe + 24
        if struct.unpack_from("<H", self.data, self.optional)[0] != 0x20B:
            raise ValueError("not PE32+")
        table = self.optional + optional_size
        self.sections: list[Section] = []
        for index in range(count):
            off = table + 40 * index
            name = self.data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
            virtual_size, va, raw_size, raw_offset = struct.unpack_from("<IIII", self.data, off + 8)
            characteristics = struct.unpack_from("<I", self.data, off + 36)[0]
            self.sections.append(Section(name, va, virtual_size, raw_offset,
                                         raw_size, characteristics))

    def rva_to_offset(self, rva: int) -> int:
        for section in self.sections:
            if section.va <= rva < section.va + max(section.virtual_size, section.raw_size):
                return section.raw_offset + rva - section.va
        raise ValueError(f"unmapped RVA 0x{rva:x}")

    def exports(self) -> dict[str, int]:
        export_rva = struct.unpack_from("<I", self.data, self.optional + 112)[0]
        off = self.rva_to_offset(export_rva)
        fields = struct.unpack_from("<IIHHIIIIIII", self.data, off)
        _, _, _, _, _, base, functions, names, eat_rva, names_rva, ordinals_rva = fields
        eat = self.rva_to_offset(eat_rva)
        name_table = self.rva_to_offset(names_rva)
        ordinal_table = self.rva_to_offset(ordinals_rva)
        out: dict[str, int] = {}
        for i in range(names):
            name_rva = struct.unpack_from("<I", self.data, name_table + i * 4)[0]
            name_off = self.rva_to_offset(name_rva)
            end = self.data.index(0, name_off)
            name = self.data[name_off:end].decode("ascii")
            ordinal = struct.unpack_from("<H", self.data, ordinal_table + i * 2)[0]
            if ordinal >= functions:
                raise ValueError(f"bad export ordinal {base + ordinal}")
            out[name] = struct.unpack_from("<I", self.data, eat + ordinal * 4)[0]
        return out


def find_gate(pe: PE) -> list[tuple[int, int]]:
    matches: list[tuple[int, int]] = []
    for section in pe.sections:
        if not (section.characteristics & 0x20000000):
            continue
        blob = pe.data[section.raw_offset:section.raw_offset + section.raw_size]
        for i in range(0, max(0, len(blob) - 48)):
            if blob[i:i + 4] != bytes.fromhex("83 79 14 03"):
                continue
            for j in range(4, 40):
                probe = blob[i + j:i + j + 4]
                if (len(probe) >= 3 and probe[:2] == b"\x0f\x9d" and
                        probe[2] & 0xF8 == 0xC0):
                    matches.append((section.va + i + 3, section.va + i + j))
                    break
                if (len(probe) == 4 and probe[0] & 0xF0 == 0x40 and
                        probe[1:3] == b"\x0f\x9d" and probe[3] & 0xF8 == 0xC0):
                    matches.append((section.va + i + 3, section.va + i + j))
                    break
    return matches


def find_config(pe: PE, init_rva: int) -> int | None:
    start = pe.rva_to_offset(init_rva)
    for i in range(90):
        if pe.data[start + i:start + i + 3] != b"\x48\x8d\x0d":
            continue
        displacement = struct.unpack_from("<i", pe.data, start + i + 3)[0]
        target = init_rva + i + 7 + displacement
        for section in pe.sections:
            if (section.characteristics & 0x80000000 and section.va <= target and
                    target + 0x12A6 <= section.va + section.virtual_size):
                return target
    return None


def count_fatbins(data: bytes) -> tuple[int, int, int]:
    magic = b"\x50\xed\x55\xba"
    valid = sm89 = sm120 = 0
    cursor = 0
    while True:
        cursor = data.find(magic, cursor)
        if cursor < 0:
            break
        if cursor + 16 <= len(data):
            header = struct.unpack_from("<H", data, cursor + 6)[0]
            payload = struct.unpack_from("<Q", data, cursor + 8)[0]
            if 0x10 <= header <= 0x100 and payload <= 64 * 1024 * 1024 and cursor + header + payload <= len(data):
                valid += 1
                end = cursor + header + payload
                entry = cursor + header
                while entry + 0x20 <= end:
                    eh = struct.unpack_from("<I", data, entry + 4)[0]
                    ds = struct.unpack_from("<I", data, entry + 8)[0]
                    arch = struct.unpack_from("<I", data, entry + 0x1C)[0]
                    sm89 += arch == 0x59
                    sm120 += arch == 0x78
                    if not 0x20 <= eh <= 0x400 or entry + eh + ds > end:
                        break
                    entry = (entry + eh + ds + 7) & ~7
        cursor += 4
    return valid, sm89, sm120


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("dll", type=Path)
    args = parser.parse_args()
    pe = PE(args.dll)
    exports = pe.exports()
    init_rva = exports.get("NVP_Init_D3D")
    gates = find_gate(pe)
    config = find_config(pe, init_rva) if init_rva is not None else None
    fatbins, sm89, sm120 = count_fatbins(pe.data)
    print(f"file={args.dll}")
    print(f"sha256={hashlib.sha256(pe.data).hexdigest()}")
    print(f"NVP_Init_D3D={f'+0x{init_rva:x}' if init_rva is not None else 'missing'}")
    print(f"gate_candidates={len(gates)} " + " ".join(
        f"cmp_imm=+0x{cmp_rva:x},setge=+0x{setge_rva:x}" for cmp_rva, setge_rva in gates))
    print(f"config={f'+0x{config:x}' if config is not None else 'missing'}")
    print(f"fatbins={fatbins} sm89_entries={sm89} sm120_entries={sm120}")
    if init_rva is None or len(gates) != 1 or config is None:
        raise SystemExit(2)


if __name__ == "__main__":
    main()

