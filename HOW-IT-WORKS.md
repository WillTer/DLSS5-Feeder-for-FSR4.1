# How It Works: FSR 4.1 in a 32-bit DirectX 9 Game (2003) Without Engine Source

This document explains the complete architecture of the fsr41-feeder project so it can be adapted to other legacy D3D9 games. It covers every layer, why each exists, what breaks without it, and how to debug when porting.

---

## Architecture Overview

```
SpellForce.exe (32-bit, D3D9, 2003 engine)
 │
 ├─ dinput8.dll (our proxy)                    ← layer 1: module loading control
 │   ├─ diverts d3d9/ddraw/d3d8 loads to local dgVoodoo copies
 │   ├─ patches Direct3DCreate9/9Ex entry points (5-byte JMP)
 │   ├─ wraps IDirect3D9 vtable: sanitized mode list ("classic diet")
 │   └─ crash tracer (VEH), diagnostic hooks (ExitProcess, MessageBoxA)
 │
 ├─ dgVoodoo2 2.87.6 (D3D9.dll, local copy)   ← layer 2: API translation
 │   ├─ translates D3D9 calls → D3D11 device + DXGI swapchain
 │   ├─ OutputAPI = d3d11_fl11_0               ← must be D3D11, not D3D12
 │   └─ PresentationModel = flip_discard       ← must be flip model, not auto/BitBlt
 │
 ├─ ReShade 6.8 (dxgi.dll, addon build)        ← layer 3: hook point + effects
 │   ├─ intercepts the DXGI swapchain dgVoodoo creates
 │   ├─ creates a D3D11 effect runtime on it
 │   ├─ LumeniteFX Kernel → optical-flow motion vectors
 │   └─ DLSS5_Feed.fx → MV validation, depth normalisation
 │
 ├─ dlss5-feed.addon32 (patched upstream)      ← layer 4: frame transport (game side)
 │   ├─ skips non-D3D11 runtimes (waits for D3D11, doesn't self-disable)
 │   ├─ copies color/depth/MV into shared NT-handle textures
 │   └─ signals via cross-process fences to the host
 │
 └─ host64/dlss5-feed-host64.exe (our build)   ← layer 5: ML upscaler
     ├─ own D3D12 device, no window, no ReShade
     ├─ amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_upscaler_dx12.dll
     ├─ ffxCreateContext → FSR 4.1 (provider "4.1.2")
     └─ ffxDispatch per frame → shared Output texture → blit back
```

---

## Layer 1: The dinput8 Proxy (`tools/dinput-redirect/`)

### Why dinput8?

The game imports exactly one function from `DINPUT8.dll` (DirectInput8Create, by name). A local DLL with that name loads instead of the system one, and its `DllMain` runs before any engine code — the earliest possible hook point.

### What it does, in order of execution

#### 1a. IAT patch (at DLL_PROCESS_ATTACH)

Walks the game exe's import table, finds `kernel32.dll` entries for `LoadLibraryA` and `GetProcAddress`, and redirects them to our wrappers. This catches all the engine's dynamic loading.

**Critical bug to avoid**: the loop must scan the ENTIRE kernel32 thunk array — an early `return` after the first match means only one function gets hooked. This cost hours of debugging.

```cpp
// Correct: scan all thunks, patch both functions
for (int i = 0; thunk[i].u1.Function != 0; ++i) {
    if (strcmp(fn, "LoadLibraryA") == 0) { /* patch */ }
    else if (strcmp(fn, "GetProcAddress") == 0) { /* patch */ }
    // NO return here — keep scanning!
}
```

#### 1b. MinHook inline hooks (armed at first DirectInput8Create call)

Global hooks on `LoadLibraryA/W/ExA/ExW`. These catch loads the IAT hook misses (calls through GetProcAddress'd function pointers, other threads).

**Important**: MinHook must NOT be initialized in DllMain (loader-lock deadlock risk). Initialize in the first DirectInput8Create call — by then the loader lock is released.

#### 1c. Legacy DirectX DLL diversion

All loads of `d3d9.dll`, `d3d8.dll`, `ddraw.dll`, `d3dimm.dll` — whether by bare name or absolute system path — are redirected to the copies in the game folder. This is necessary because the engine builds `GetSystemDirectoryA() + "\d3d9.dll"` and loads that explicitly.

```cpp
static bool RedirectLegacyName(const char *file, char *out, size_t cb)
{
    // matches by filename suffix, builds full path to our local copy
    // returns false for everything else (pass through)
}
```

#### 1d. Direct3DCreate9 entry-point patch (behind `FSR41_PATCH_D3D9=1`)

The engine resolves `Direct3DCreate9` by walking the d3d9.dll export table manually — bypassing GetProcAddress entirely. The only interception that works is patching the function's first 5 bytes with a JMP to our wrapper.

```cpp
// Generic trampoline: disassemble instructions until ≥5 bytes displaced
// (works with ANY prologue, not just the classic hot-patch pattern)
hde32_disasm() to find instruction boundaries
memcpy displaced bytes to RWX trampoline (VirtualAlloc, NOT static array!)
append JMP back to original+N
write JMP to our wrapper at the original entry point
```

**Critical bug to avoid**: the trampoline MUST be in executable memory. A `static uint8_t` array lives in `.data` — non-executable under DEP. The process dies instantly with no useful diagnostics. Use `VirtualAlloc(PAGE_EXECUTE_READWRITE)`.

**Only applied when `FSR41_PATCH_D3D9=1`** is set in the environment. Some engines checksum the loaded d3d9 module and abort if its bytes are modified. Default off; enable per-game after testing.

#### 1e. IDirect3D9 vtable wrap ("classic diet")

After Direct3DCreate9 returns an object, we swap its vtable pointer to a clone with these methods replaced:

| Slot | Method | What we return |
|------|--------|---------------|
| 4 | GetAdapterCount | Always 1 (hide secondary monitors) |
| 5 | GetAdapterIdentifier | Pass-through (no truncation needed) |
| 6 | GetAdapterModeCount | Curated list size (typically 10) |
| 7 | EnumAdapterModes | Classic 4:3 modes + native resolution |
| 8 | GetAdapterDisplayMode | Pass-through |

**Why**: a modern desktop reports 50+ modes per format through dgVoodoo. The engine's fixed-size arrays (2003-era) overflow, producing garbage like `resolution : x:65536 y:0` followed by an assert in `AppSession.cpp`. A second (portrait) monitor as adapter 1 confuses the engine's adapter walk. The curated list must include the display's native resolution (the engine picks it as default on newer builds).

```cpp
static const ClassicMode kClassicModes[] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 },
    { 1280, 960 }, { 1280, 1024 }, { 1600, 1200 },
    { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },  // include native!
};
```

#### 1f. Diagnostic instrumentation (always on, gated to stay quiet)

- **Vectored Exception Handler**: catches all exceptions (AV, stack overflow, fastfail), logs EIP/registers/stack/module map. Essential for diagnosing crashes in packed executables where static analysis fails.
- **ExitProcess tracer**: MinHook on kernel32!ExitProcess, logs a stack backtrace. Distinguishes "the engine chose to quit" from "it crashed".
- **MessageBoxA tracer**: logs the text and caller of any error dialog the engine shows.

**Critical bug to avoid**: the module-map string (~8 KB) must NOT go through `sprintf_s` into a 584-byte buffer — it trips `__fastfail(RANGE_CHECK)` and kills the game before its own code runs. Route oversized payloads directly to `fputs`.

---

## Layer 2: dgVoodoo Configuration

The single most fragile part of the chain. Two settings must be exactly right:

### OutputAPI = d3d11_fl11_0 (NOT d3d12, NOT bestavailable)

- **d3d11_fl11_0**: creates a D3D11 device + DXGI swapchain → ReShade's dxgi.dll intercepts it → D3D11 runtime → feeder connects. ✅
- **d3d12**: creates a D3D12 device → no DXGI swapchain path ReShade can hook → runtime churn, feeder never connects. ❌
- **bestavailable**: on modern systems picks d3d12 → same problem. ❌

### PresentationModel = flip_discard (NOT auto, NOT bitblt)

- **flip_discard**: creates a DXGI flip-model swapchain → ReShade intercepts, vsync works. ✅
- **auto**: falls back to BitBlt on the D3D9Ex path → no DXGI swapchain to intercept, feeder can't connect. vsync works. ❌ for feeder
- **bitblt**: same problem as auto. ❌ for feeder

**The combination `d3d11_fl11_0` + `flip_discard` is the only one that gives BOTH a hookable swapchain AND vsync.** An invalid value (typo like `flipdiscard` without underscore) is silently ignored and falls back to a default that may or may not work — always verify the exact spelling.

### Other settings that matter

```ini
[DirectX]
DisableAndPassThru = false       ; must be false: we want translation, not passthrough
VRAM = 1GB                       ; default 256MB causes fake OOMs
VideoCard = internal3D           ; generic emulation, don't set to real GPU name

[General]
FullScreenMode = true            ; exclusive fullscreen works better with the chain
dgVoodooWatermark = false        ; turn off after verifying dgVoodoo is active
```

---

## Layer 3: ReShade

Installed as `dxgi.dll` (full add-on build, NOT the standard build). The headless installer (`ReShade_Setup_Addon.exe --headless --api d3d11 <exe>`) usually works, but may produce `d3d11.dll` instead of `dxgi.dll` — rename if needed.

### ReShade2.ini for multi-swapchain processes

dgVoodoo may create a dummy 1×1 swapchain before the real one. ReShade gives the first runtime `ReShade.ini` and the second `ReShade2.ini`. Without `ReShade2.ini`, the real swapchain's runtime has no preset and never renders the feeder's technique.

```ini
[GENERAL]
EffectSearchPaths=.\reshade-shaders\Shaders
TextureSearchPaths=.\reshade-shaders\Textures
PresetPath=.\ReShadePreset.ini

[DEPTH]
DepthCopyBeforeClears=1
```

### Preset: per-effect preprocessor definition

`DLSS5_MV_PROVIDER=3` (LumeniteFX Kernel) must be in the `[DLSS5_Feed.fx]` section of `ReShadePreset.ini`, NOT in `[GENERAL]`. Setting it globally does nothing.

```ini
Techniques=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx

[DLSS5_Feed.fx]
PreprocessorDefinitions=DLSS5_MV_PROVIDER=3
```

---

## Layer 4: The 32-bit Add-on (`dlss5-feed.addon32`)

Built from the upstream DLSS5-Feeder source with ONE patch: the device-API check in the render callback.

**Original** (self-disables on D3D9):
```cpp
if (dev_api->get_api() != reshade::api::device_api::d3d11)
{ FeedDisable("only Direct3D 10/11/..."); return; }
```

**Patched** (skips D3D9 frames, waits for D3D11):
```cpp
if (dev_api->get_api() != reshade::api::device_api::d3d11)
{
    static bool said = false;
    if (!said) { said = true; Log("skipping non-D3D11 runtime; waiting for D3D11"); }
    return;  // don't disable, just skip this frame
}
```

**Why**: under dgVoodoo, ReShade creates a D3D9 runtime on the game's D3D9 device AND a D3D11 runtime on the DXGI swapchain. The D3D9 one appears first. Without this patch, the addon sees D3D9, self-disables, and never sees the D3D11 runtime that follows.

---

## Layer 5: The 64-bit Host (`host64/dlss5-feed-host64.exe`)

Rewritten from the upstream NGX-based host. Calls FidelityFX `ffx_api` directly:

```cpp
ffxCreateContextDescUpscale  createUpscale = {};
createUpscale.flags         = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
createUpscale.maxRenderSize = {w, h};
createUpscale.maxUpscaleSize = {w, h};  // == render for NativeAA; > for SR mode

ffxCreateBackendDX12Desc    backend = {};
backend.device = pD3D12Device;

ffxCreateContextDescUpscaleVersion ver = {};
ver.version = FFX_UPSCALER_VERSION;  // selects FSR 4.1

// chain: upscale → version → backend
upscale.header.pNext = &ver.header;
ver.header.pNext = &backend.header;
ffxCreateContext(&ctx, &upscale.header, nullptr);
```

Per frame:
```cpp
ffxDispatchDescUpscale d = {};
d.commandList = pCmdList;
d.color = ffxApiGetResourceDX12(pColor, FFX_API_RESOURCE_STATE_COMPUTE_READ);
d.depth = ffxApiGetResourceDX12(pDepth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
d.motionVectors = ffxApiGetResourceDX12(pMV, FFX_API_RESOURCE_STATE_COMPUTE_READ);
d.output = ffxApiGetResourceDX12(pOutput, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
d.jitterOffset = {0, 0};
d.motionVectorScale = {1, 1};  // pixel-space MVs
d.renderSize = {w, h};
d.upscaleSize = {w, h};
d.reset = (frame == 0);
ffxDispatch(&ctx, &d.header);
```

The host verifies the selected provider via `ffxQueryGetProviderVersion` — if it says "3.1.6" instead of "4.1.2", the driver doesn't support FSR 4.1 on this GPU.

### IPC protocol (unchanged from upstream)

- Named pipe `\\.\pipe\dlss5-feed.<pid>` (byte-mode, 256-byte input buffer)
- Game creates 4 shared NT-handle textures (Color, Output, Depth, MV)
- Host creates 2 shared fences (in/out), duplicates into game process
- Per frame: game copies to shared textures, signals fence_in, sends message
- Host waits, evaluates, signals fence_out; game blits output to backbuffer
- Pipe break = graceful disable (game renders normally without the feeder)

---

## Windows AppCompat Shim

**The DWM8And16BitMit shim** (automatically applied by Windows to old games) wraps the window procedure and calls a NULL callback under our hook chain, crashing the game with an access violation that the engine's crash dialog reports but our VEH can't meaningfully decode.

**Fix**: set the Layers entry to `~` (disables all shims for the exe):

```powershell
# HKCU (no admin needed)
New-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers' `
  -Name '<full path to exe>' -Value '~' -PropertyType String -Force

# HKLM (needs admin — if the entry was also set there)
Remove-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers' `
  -Name '<full path to exe>'
```

Verify: `apphelp.dll` should NOT appear in the process's loaded modules.

---

## Antivirus

Kaspersky (and likely other AVs) delete the unsigned `dinput8.dll` — a DLL named after a system DLL that hooks LoadLibrary and patches vtables is textbook malware heuristics. **The game folder must be excluded** before installing. If the proxy disappears between runs, check AV first.

---

## Debugging Guide (for adapting to other games)

### Symptom: game renders but no FSR effect visible

1. Check `dlss5-feed.log` — is the addon attached? Does it say "effect runtime initialised"?
2. Check for "skipping a non-D3D11 runtime" — means only D3D9 runtime exists
3. Check `ReShade.log` — is a DXGI swapchain being intercepted?
4. Try `PresentationModel = flip_discard` — the default `auto` often uses BitBlt
5. Try creating `ReShade2.ini` — multiple swapchains = multiple runtimes

### Symptom: game crashes at startup with dgVoodoo

1. Check `Documents\<Game>\Log.txt` (or equivalent) for garbage resolution values like `x:65536`
2. If garbage → the classic-diet mode-list wrap is needed (vtable slots 6-8)
3. Check for a second monitor → may need GetAdapterCount→1 wrap (slot 4)
4. Check `fsr41-redirect.log` for AV entries with stack traces

### Symptom: game runs but exits silently after 1-2 minutes

1. Check `fsr41-redirect.log` for `ExitProcess` trace with engine stack frames
2. This is usually the engine asserting on a D3D call that returned an unexpected value
3. Disassemble the exit frames (capstone + `tools/disasm.py`) to find the failing check

### Symptom: settings screen crashes

The engine's settings UI may overflow a fixed buffer with a long adapter name. The classic-diet wrap usually fixes this too (fewer modes = less data to copy). If not, truncate `Description[28]` in `GetAdapterIdentifier`.

### Symptom: works on one game version, crashes on another

Engine builds differ in their D3D usage (classic vs Ex, mode parsing, anti-wrapper checks). Each build may need different wrap parameters. The 1.54 and 1.61 SpellForce builds needed different handling.

---

## Porting Checklist

- [ ] Game is 32-bit D3D9 (check with `file <exe>` or PE header)
- [ ] Game loads d3d9.dll dynamically (check import table — if static, different approach needed)
- [ ] Identify the game's config storage (registry, config.lua, ini) for resolution settings
- [ ] Install dinput8 proxy (build from `tools/dinput-redirect/`)
- [ ] Install dgVoodoo2 (full set: D3D9, D3D8, D3DImm, DDraw + conf + Cpl)
- [ ] Configure dgVoodoo: `OutputAPI=d3d11_fl11_0`, `PresentationModel=flip_discard`
- [ ] Install ReShade addon build as dxgi.dll (headless or manual)
- [ ] Create ReShade.ini + ReShadePreset.ini + ReShade2.ini
- [ ] Install LumeniteFX Kernel shaders + framework headers
- [ ] Build + install addon32 (with the D3D9-skip patch)
- [ ] Build + install host64 + AMD FFX DLLs
- [ ] Disable AppCompat shim (`~` in Layers)
- [ ] Add game folder to AV exclusions
- [ ] Set `FSR41_PATCH_D3D9=1` environment variable for the entry-point patch
- [ ] Test: launch, check host64 log for `provider "4.1.2"`, check feed log for frames
- [ ] Tune: resolution via game config, work_resolution/work_upscale via dlss5-feed.cfg

---

## Key Files in This Repository

| Path | Purpose |
|------|---------|
| `tools/dinput-redirect/dinput8_redirect.cpp` | The proxy (layer 1) — all hooks, wraps, diagnostics |
| `tools/dinput-redirect/build.bat` | Builds to `build\dinput8.dll` |
| `host/dlss5-feed-host64.cpp` | The 64-bit FSR 4.1 host (layer 5) |
| `host/build-host.bat` | Builds to `build\dlss5-feed-host64.exe` + copies AMD DLLs |
| `src/dlss5-feed32.cpp` | Upstream addon32 with the D3D9-skip patch (layer 4) |
| `tools/build-addon32.bat` | Builds to `build\dlss5-feed.addon32` |
| `shaders/DLSS5_Feed.fx` | MV validation shader (unchanged from upstream) |
| `deploy/fetch-deps.ps1` | Downloads all dependencies to `deploy\` cache |
| `deploy/fsr41/install.bat` | Assembles the game folder from `build\` + `deploy\` |
| `deploy/fsr41/uninstall.bat` | Removes exactly what install.bat adds |
| `tools/ffx-smoke/` | Standalone FSR 4.1 initialization proof (run first on new hardware) |
| `tools/disasm.py` | Capstone-based disassembler with instruction-boundary alignment |
| `tools/minidump.py` | WOW64-aware minidump parser (exception + stack scan) |
| `tools/modules.ps1` | 32-bit module list (must run from SysWOW64 PowerShell) |
| `tools/snap.ps1` | Desktop screenshot capture |
| `README-FSR41.md` | User-facing documentation |

---

## Hardware / Driver Requirements

- **GPU**: AMD Radeon RX 6000/7000/9000 (RDNA 2+; FSR 4.1 uses shader-based INT8 inference on RDNA 2)
- **Driver**: AMD Adrenalin 26.10.1 or newer (brought FSR 4.1 to RDNA 2)
- **OS**: Windows 10/11 (D3D11 + DXGI required)
- **Performance**: ~2.7 ms GPU per frame at 1600×1200 NativeAA, ~8 ms at 1080p→4K SR mode on RX 6750 XT

---

## Known Limitation: SR Mode on Legacy Content

Setting `work_upscale=2` with `work_resolution<100` (SR mode: render lower → ML upscale to native) produces **visible artifacts on 2003-era games**:

- **Pixelation**: the ML model was trained on modern game content with detailed textures; on hard-edged low-detail 2003-era art, its reconstruction is worse than a simple bilinear at full resolution
- **Disappearing small objects**: the optical-flow MVs (LumeniteFX) cannot track small fast-moving objects (game cursor, 16×16 px at 63% scale); the temporal reconstruction drops them between frames
- **Temporal jitter**: the synthetic Halton jitter (needed for SR reconstruction) combined with noisy optical-flow MVs causes unstable frame-to-frame accumulation, visible as shimmer

**Recommendation**: use `work_upscale=0` + `work_resolution=100` (NativeAA 1:1) — full-resolution render with FSR 4.1 as pure ML anti-aliasing/edge enhancement. No temporal artifacts, no resolution loss, ~12 ms GPU at 4K on RX 6750 XT. SR mode is only worth trying on games with modern, high-detail rendering where the ML model has enough information to work with.
