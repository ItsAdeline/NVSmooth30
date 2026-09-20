#!/usr/bin/env python3
"""Repository-level checks that do not require the Windows SDK."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    listed = re.findall(r"\b(?:src|include)/[A-Za-z0-9_./-]+\.(?:cpp|hpp|def)", cmake)
    for relative in listed:
        require((ROOT / relative).is_file(), f"CMake source missing: {relative}")

    definitions = (ROOT / "src/version_proxy.def").read_text(encoding="utf-8")
    proxy = (ROOT / "src/version_proxy.cpp").read_text(encoding="utf-8")
    aliases = re.findall(r"=([A-Za-z0-9_]+)", definitions)
    for alias in aliases:
        require(alias in proxy, f"proxy export implementation missing: {alias}")

    source = "\n".join(path.read_text(encoding="utf-8") for path in ROOT.rglob("*.cpp"))
    require("0x06005604" in source, "correct SM86 ELF flags constant missing")
    require("0xBA55ED50" in source, "CUDA fatbin magic missing")
    require("cuModuleLoadDataEx" not in source,
            "cuModuleLoadDataEx must not be redirected to the incompatible base ABI")
    lowered = source.lower()
    if "0x7fb628" in lowered or "0x1348d0" in lowered:
        require("kReferenceResolverPrefix" in source and
                "reference_layout" in source and
                "0xc41f" in lowered and "0xc437" in lowered and
                "0x7f0cd0" in lowered,
                "driver-specific CUDA compatibility fallback is not sufficiently guarded")

    nvp = (ROOT / "src/nvpresent.cpp").read_text(encoding="utf-8")
    bridge = (ROOT / "src/d3d11_bridge.cpp").read_text(encoding="utf-8")
    require("std::byte{0x83}" in nvp and "std::byte{0x79}" in nvp and
            "std::byte{0x14}" in nvp and "std::byte{0xc6}" in nvp,
            "reference NvPresent gate signature is missing")
    require("no SM86 CUDA device found" not in nvp,
            "early CUDA capability hard-gate was reintroduced")
    require("D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX" not in bridge and
            "AcquireSync" not in bridge and "ReleaseSync" not in bridge,
            "keyed-mutex bridge path differs from the reference")
    require("D3D11_RESOURCE_MISC_SHARED_NTHANDLE" in bridge and
            "D3D11_RESOURCE_MISC_SHARED" in bridge,
            "reference shared-resource fallback is missing")
    require("shadow_desc.Flags = 0" in bridge,
            "shadow swapchain flags differ from the reference")
    require("table[19]" in bridge and "table[20]" in bridge,
            "validated NvPresent wrapper activation is missing")
    require("find_delay_import_slot" in nvp and "find_data_pointer_slots" in nvp,
            "robust direct CUDA interception fallback is missing")
    require("D3D12CreateDevice(nullptr" in bridge,
            "reference default-adapter D3D12 creation order is missing")

    for path in ROOT.rglob("*.*"):
        if path.suffix.lower() in {".dll", ".exe", ".lib", ".pdb"}:
            raise SystemExit(f"FAIL: binary unexpectedly included: {path.relative_to(ROOT)}")

    if len(sys.argv) > 1:
        completed = subprocess.run(
            [sys.executable, str(ROOT / "tools/inspect_nvp.py"), sys.argv[1]],
            capture_output=True,
            text=True,
            check=False,
        )
        print(completed.stdout, end="")
        if completed.stderr:
            print(completed.stderr, end="", file=sys.stderr)
        require(completed.returncode == 0, "reference NvPresent inspection failed")
        expected = [
            line for line in (ROOT / "tests/reference_expected.txt").read_text(
                encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        ]
        for line in expected:
            require(line in completed.stdout, f"reference fingerprint changed: {line}")

    print(f"PASS: {len(listed)} CMake paths, {len(aliases)} proxy exports, guarded reference-parity invariants")


if __name__ == "__main__":
    main()
