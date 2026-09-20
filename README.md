# NVSmooth30

NVSmooth30 is a clean-room, source-available compatibility experiment for
running NVIDIA's `NvPresent64.dll` Smooth Motion path on SM86/RTX 30-series
hardware. It is not NVIDIA software and contains no NVIDIA binaries or copied
source code.

## Status and safety

This is an **alpha interoperability project**. It was designed against the
observed behavior of one reference `NvPresent64.dll` build, SHA-256:

`cd395d58f41c6e393c31a9898f2be3da83f7bc109a2228935c23e8c89b944c15`

It does not hard-code that build's internal RVAs. Runtime scanners require the validated compare/capability structure used by
the reference path and abort without patching when that structure is unknown. New NVIDIA versions can still change semantics, kernel formats, or
presentation behavior.

The CUDA step is conservative metadata retargeting, not a general SASS binary
translator. It is suitable only while the shipped SM89 kernels use instructions
that are valid on SM86. Unknown fatbin layouts are rejected instead of edited.

Do not use this in competitive or anti-cheat-protected games. Keep a backup of
every replaced file. A driver reset, game crash, corrupted frame, or black
screen remains possible.

## Building

Requirements:

- 64-bit Windows 10 or 11;
- Visual Studio 2022 with **Desktop development with C++**;
- Windows 10/11 SDK;
- CMake 3.24 or newer.

Run:

```bat
build_release.bat
```

The result is `build\Release\version.dll`. Copy it beside the game's main
executable. Do not copy `NvPresent64.dll`; NVSmooth30 loads the installed driver
copy from DriverStore.

## Configuration

Set environment variables before launching the game:

| Variable | Default | Purpose |
|---|---:|---|
| `SM86_ENABLE_OSD` | `0` | Draw the lightweight status OSD; F11 toggles it. |
| `SM86_ENABLE_D3D11_BRIDGE` | `1` | Enable the experimental D3D11-to-D3D12 bridge; set to `0` to disable it. |
| `SM86_FORCE_VSYNC` | `0` | Force swapchain sync interval 1. |
| `SM86_DIAGNOSTICS` | `0` | Reserve verbose diagnostic behavior. |
| `SM86_LOW_LATENCY` | `1` | Request maximum frame latency 1 when supported. |
| `SM86_HALF_REFRESH_CAP` | `0` | Pace base frames at half the active refresh rate. |
| `SM86_BASE_FPS_CAP` | `0` | Explicit base-frame cap; zero disables it. |
| `SM86_BRIDGE_LINEARIZE` | `0` | Map sRGB source formats to linear for the bridge; default keeps source format. |
| `SM86_NVPRESENT_PATH` | auto | Override the full path to `NvPresent64.dll`. |

Example launcher:

```bat
@echo off
set SM86_ENABLE_D3D11_BRIDGE=1
set SM86_ENABLE_OSD=1
start "" "Game.exe"
```

Diagnostics are written to `nvsmooth30.log` beside the game executable.

## Offline driver inspection

The regression inspector does not load or modify the DLL:

```bat
py tools\inspect_nvp.py "C:\path\to\NvPresent64.dll"
```

For the reference build it should find the validated `cmp [rcx+14h], 2/3` +
`SETGE SIL` capability structure, `NVP_Init_D3D`, and one writable configuration
target. Unknown layouts are intentionally unsupported until reviewed.

## Architecture

1. The version proxy loads the real Windows `version.dll` from System32.
2. The NvPresent module is located and inspected as a normal PE image.
3. The CUDA load path is intercepted through a named import or NvPresent's
   standard `GetProcAddress` import.
4. Only valid SM89 cubin entries are copied and retargeted to SM86; SM120 and
   unknown entries remain untouched.
5. Gate/config changes are applied as a transaction and restored on failure.
6. Shared DXGI vtables expose newly created game swapchains to the presentation
   path. D3D11 can optionally copy through a two-buffer D3D12 shadow swapchain.
7. The D3D11 bridge mirrors the reference shared-resource path: NT-handle sharing
   with a legacy SHARED fallback, D3D11 event-query synchronization, and validated
   NvPresent wrapper activation when the private wrapper object is discovered.

## Clean-room note

The project is organized from observed inputs, outputs, public PE/DXGI/D3D/CUDA
interfaces, and independently written code. It is not a variable-renamed
decompilation. The external binary that motivated the experiment is not
included and its authorship is not claimed.

## License

The independently written source in this archive is released under the MIT
License. NVIDIA, CUDA, DirectX, and Windows are trademarks of their respective
owners. No NVIDIA or third-party binaries are redistributed.
