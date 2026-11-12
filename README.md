# GhostXR

An in-process Original Xbox HLE compatibility runtime for StarCraft: Ghost on Windows (D3D11 / WASAPI).

## Overview

`GhostXR` reserves the Original Xbox virtual address space (`0x00401000..0x00950FFF`) inside a 32-bit Windows PE executable (`GhostXR.exe`), maps the unmodified `Ghost.exe` Xbox sections directly in-process without binary modification on disk, and bridges Xbox hardware/OS subsystems to native Windows APIs:

- **Xbox Kernel & XAPI**: Thread-local KPCR/TLS (`FS:[0x28]` redirected via Windows TLS), NT synchronization objects, virtualized drive-letter sandboxing (`D:`, `T:`, `U:`, `Z:`, etc. under `GhostXR_Data/`), and 733.333 MHz TSC / 3.375 MHz ACPI timer virtualization.
- **NV2A / Xbox D3D8 -> Direct3D 11**: Programmable vertex shaders (`.xvu`), register combiners (`.xpu`), fixed-function pipeline, swizzled/linear/palettized/DXT textures, render-to-texture surface aliases, and first-run DXBC shader precompilation (`GhostXR_Data/shaders/*.cso`).
- **MCPX / DirectSound -> WASAPI**: Xbox ADPCM & PCM voices, 3D positional audio, I3DL2/submix routing, and asynchronous streaming packets mixed in float32 via shared-mode WASAPI.
- **XPP / Input**: XInput controller hotplugging, vibration, and customizable keyboard/mouse bindings via a built-in pre-launch Win32 setup dialog.

## Building

Requires **Visual Studio 2022+** (with the C++ Desktop workload and 32-bit x86 / `Win32` toolset) and **CMake 3.20+**.

```powershell
cmake --preset x86-release
cmake --build --preset x86-release
```

Output binary: `build/bin/GhostXR.exe`

## Running

1. Launch `build/bin/GhostXR.exe`.
2. Select the directory containing the Original Xbox `Ghost.exe` and game assets (read-only; never modified by `GhostXR`).
3. Configure your preferred Direct3D 11 adapter and optional keyboard controls, then click **Launch**.
4. User configuration, precompiled D3D11 shaders, and virtualized Xbox saves/caches are stored in `GhostXR_Data/` next to `GhostXR.exe`.
