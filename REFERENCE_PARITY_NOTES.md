# Reference-parity changes

This revision aligns the independently written NVSmooth30 source with the runtime behavior observed in the working reference DLL supplied for comparison.

## NvPresent bootstrap

- Gate scanner now requires the reference structural signature: `83 79 14 03` (or already-patched immediate `02`) followed within 40 bytes by `SETGE SIL` in either its 4-byte REX form or 3-byte form.
- The `3/4` distinction is the instruction length of the capability patch, not a required number of scanner candidates.
- Removed early CUDA-device capability gating. The direct `nvcuda.dll!cuModuleLoadData` IAT interception is primary; resolved-IAT and `GetProcAddress` interception remain compatibility fallbacks.
- The fatbin parser remains the safety boundary: only structurally valid SM89 entries are retargeted to SM86.
- Config bytes `+0x4c` and `+0xe9` are logged before modification, set before `NVP_Init_D3D`, and reasserted after a successful init.

## D3D11 -> D3D12 bridge

- Shadow chain now matches the working descriptor: 2 buffers, flip-discard, stretch scaling, unspecified alpha, `Flags = 0`.
- Removed waitable-object/`SetMaximumFrameLatency(1)` behavior from the private shadow chain.
- Shared D3D11 texture now tries `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` first, then falls back to legacy `D3D11_RESOURCE_MISC_SHARED`.
- Removed `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` and all `AcquireSync`/`ReleaseSync` calls.
- D3D11 producer completion uses the event query + `Flush`/`GetData(DONOTFLUSH)` path with a 500 ms timeout.
- D3D12 fence wait matches the reference blocking behavior.
- Bridge minimum dimension is 480 pixels in either axis instead of requiring width >= 854.

## NvPresent wrapper activation

- A candidate behind the shadow swapchain is accepted for private activation only when its vtable has readable slots 19 and 20 and both methods point inside the loaded `NvPresent64.dll` image.
- Those two methods are called with `true`, matching the working wrapper activation sequence.
- A saved NvPresent Present trampoline is only an activity signal; it no longer marks the private wrapper as confirmed.
- Real wrapper probing continues for up to 64 shadow Presents, so lazy attachment can still be discovered and enabled.

## Validation

Run:

```bat
py tools\validate_project.py
```

Then build on 64-bit Windows with Visual Studio 2022 / Windows SDK:

```bat
build_release.bat
```

Expected output DLL:

```text
build\Release\version.dll
```

For the first runtime test, use diagnostics and keep optional features minimal:

```bat
set SM86_DIAGNOSTICS=1
set SM86_ENABLE_OSD=0
set SM86_FORCE_VSYNC=0
set SM86_ENABLE_D3D11_BRIDGE=1
```

Useful success markers include:

```text
Gate located dynamically: cmp=+0x... setge=+0x... len=3/4
cuModuleLoadData IAT found dynamically: +0x...
Config before init: ...
Config after init: ...
NVP_Init_D3D=TRUE
CreateTexture2D(SHARED_NTHANDLE) ...   (only logged on failure/fallback)
Wrapper layout discovered dynamically: ... offset=+0x18 ...
enabled NvPresent wrapper private toggles (slots 19/20)
>>> SUCCESS: NvPresent64 wrapped D3D12 shadow swapchain ...
```

## 2026-09-20 D3D11 sRGB shadow-chain fix

A runtime log exposed a parity bug in the reconstructed bridge: source format 29
(`DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`) was being passed unchanged to a
`DXGI_SWAP_EFFECT_FLIP_DISCARD` D3D12 shadow swapchain when
`SM86_BRIDGE_LINEARIZE=0`, causing `CreateSwapChainForHwnd` to return
`DXGI_ERROR_INVALID_CALL (0x887A0001)`.

Disassembly of the working reference DLL's format helper at RVA `0x79C0`
confirmed this exact safe mapping:

- 10 -> 10 (`R16G16B16A16_FLOAT`)
- 24 -> 24 (`R10G10B10A2_UNORM`)
- 28 -> 28 (`R8G8B8A8_UNORM`)
- 29 -> 28 (`R8G8B8A8_UNORM_SRGB` -> `R8G8B8A8_UNORM`)
- 87 -> 87 (`B8G8R8A8_UNORM`)
- 91 -> 87 (`B8G8R8A8_UNORM_SRGB` -> `B8G8R8A8_UNORM`)

The bridge now applies that mapping unconditionally for shadow/shared resources.

## V3: wrapper-creation / CUDA interception fix

The working V2.5 reference log shows a decisive ordering relationship that was
missing from the reconstructed bridge:

1. `NVP_Init_D3D` succeeds and the direct `cuModuleLoadData` slot is patched.
2. Bridge initialization creates a D3D12 device.
3. NvPresent loads a new batch of CUDA fatbins, which the hook rewrites from
   validated SM89 cubins to SM86.
4. `CreateSwapChainForHwnd` returns a shadow swapchain whose private NvPresent
   wrapper is already present at object offset `+0x18`.
5. The reference immediately calls wrapper vtable slots 19 and 20 with `true`.

The previous reconstruction fell back to a GetProcAddress hook and observed no
CUDA interceptions at all during bridge creation, after which the wrapper never
appeared. V3 therefore adds three direct interception paths before the
GetProcAddress fallback:

- normal named import (`nvcuda.dll!cuModuleLoadData`),
- delay-import IAT entry, and
- exact resolved-function-pointer slots in NvPresent non-executable image data.

V3 also matches the reference's `D3D12CreateDevice(nullptr, ...)` call order and
logs the selected D3D12 adapter LUID plus CUDA interception counts around device
and shadow-swapchain creation.

Expected healthy diagnostics include at least one of:

```
[nvs30] cuModuleLoadData IAT found dynamically: +0x...; direct hook installed.
[nvs30] cuModuleLoadData delay-IAT found dynamically: +0x...; direct hook installed.
[nvs30] cuModuleLoadData resolved pointer slot hooked: +0x....
```

followed by one or more successful CUDA-fatbin rewrite lines during bridge
initialization and then:

```
[nvs30-bridge] Wrapper layout discovered dynamically: ... offset=+0x18 ...
[nvs30-bridge] enabled NvPresent wrapper private toggles (slots 19/20) ...
[nvs30-bridge] >>> SUCCESS: NvPresent64 wrapped D3D12 shadow swapchain ...
```


## v4: exact reference CUDA fallback

The working `version(3).dll` was disassembled further. When its normal PE import walk fails, it validates an NvPresent resolver stub at RVA `0x1348D0` by checking the first eight bytes (`48 83 EC 28 45 33 C9 48`), calls that stub, then reads the populated CUDA dispatch/IAT slot at RVA `0x7FB628`. The earlier v3 reconstruction did not reproduce this guarded compatibility path, which is why the log fell through to the ineffective `GetProcAddress` hook and recorded `CUDA_intercepts=0->0`. v4 ports the reference fallback with image/section/signature validation before either hard-coded RVA is used. Generic import/delay-import/resolved-pointer discovery remains in place for other NvPresent versions.
