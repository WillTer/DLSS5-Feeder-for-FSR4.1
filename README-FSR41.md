# FSR 4.1 Feeder — SpellForce Platinum Edition

AMD **FSR Upscaling 4.1** (the ML upscaler) in a 32-bit DirectX 9 game from
2003 — no engine source required. This is a fork of
[jlrouzies-fr/DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder)
v1.19.1-beta.1 (MIT) with the entire NVIDIA side (NGX SDK, the
`renodx-dlss5` add-on, the host's decoy window and in-host ReShade) replaced
by direct FidelityFX `ffx_api` calls:

```
SpellForce.exe (32-bit, D3D9, Steam branch "original1.54")
 └─ dinput8.dll (our proxy) — diverts the engine's absolute-path loads to dgVoodoo
     └─ dgVoodoo2 2.87.6 (D3D9 → D3D11)
         └─ ReShade 6.8 x86 (dxgi.dll, full add-on build)
             ├─ Generic Depth → DepthBuffer
             ├─ LumeniteFX Kernel → optical-flow motion vectors
             ├─ DLSS5_Feed.fx → MV validation/camera fit, depth
             └─ dlss5-feed.addon32 ── pipe + shared NT handles + fences ──┐
                                                                         ▼
              host64\dlss5-feed-host64.exe (64-bit, no window, no ReShade)
               ├─ its own D3D12 device
               ├─ amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_upscaler_dx12.dll
               └─ FSR 4.1 → shared Output texture → blit onto the game backbuffer
```

Verified on a Radeon RX 6750 XT (RDNA 2, shader-based INT8 inference):
provider **"4.1.2"** selected (not the 3.1.6 fallback), ~2.7 ms GPU per frame
at 1600×1200 NativeAA, 18 000 frames over one session at a steady 60 fps with
the feed costing 2 % of the frame budget.

## Requirements

- A Radeon RX 6000/7000/9000 GPU and AMD Adrenalin **26.10.1 or newer**
  (the release that brought FSR 4.1 to RDNA 2);
- the Steam beta branch **`original1.54`** for the game. The current 1.62
  build crashes deterministically under dgVoodoo's translation (a bad pointer
  in the engine's dispatch table) and is not fixable from outside;
- the game folder added to **antivirus exclusions** — Kaspersky deletes the
  unsigned `dinput8.dll`, which was the top cause of "mysteriously"
  intermittent breakage during development.

## Building and installing

Nothing binary is tracked in this repository; scripts fetch and build
everything. One-time setup:

```bat
powershell -ExecutionPolicy Bypass -File deploy\fetch-deps.ps1   :: downloads to deploy\ (git-ignored)
host\build-host.bat                                             :: → build\dlss5-feed-host64.exe + AMD DLLs
tools\dinput-redirect\build.bat                                 :: → build\dinput8.dll
tools\build-addon32.bat                                         :: → build\dlss5-feed.addon32
deploy\fsr41\install.bat                                        :: assembles the game folder
```

`fetch-deps.ps1` is idempotent — re-run it any time; delete a piece under
`deploy\` to re-fetch it. It pulls the FidelityFX SDK v2.4.0 (headers are
copied into `external\ffx`, the signed AMD DLLs stay in `deploy\`), dgVoodoo2
2.87.6, the ReShade 6.8.0 installer with full add-on support, LumeniteFX
(branch `mainline`), and the ReShade framework `.fxh` headers.
`tools\ffx-smoke\build.bat` builds a standalone proof that FSR 4.1 initializes
on your GPU (`build\ffx-smoke\ffx_smoke.exe`) — run it first on any new
machine.

## Resolution — via config.lua only, never the in-game screen

The engine stores video settings as a mode **index** in `Graphic.ScreenResolution`
inside the game's `config.lua`, written by the in-game settings screen — which
can save combinations the engine then refuses to start with. The verified map:

| index | mode              | starts |
|-------|-------------------|--------|
| 0     | 800×600×16        | ✅ |
| 3     | 1024×768×32       | ✅ |
| 6     | 1280×1024×32      | ✅ |
| **8** | **1600×1200×16 — the engine's stock ceiling** | ✅ |
| 1,2,4,5,7,9 | other combinations | ❌ "unsuitable resolution" |

If the game refuses to start after someone touched the settings screen,
delete the `ScreenResolution` line from `config.lua` — the engine falls back
to its 1024×768 default. 4K rendering is unreachable natively: the engine's
UI is strictly 4:3; the community hi-res mod ships its own exe and does not
compose with this pipeline.

### Presentation geometry

With `dgVoodoo.conf [General] ScalingMode = centered_ar`, dgVoodoo presents
through a **desktop-sized (4K) swapchain** and letterboxes the 4:3 frame with
correct proportions; `ImageScaleFactor` larger than 1 produced offset/broken
placement in testing — keep it at 1. Note that `centered_ar` is also what
makes the feeder see a 4K backbuffer (see the SR caveat below).

## Diagnostics

| File | What it tells you |
|---|---|
| `host64\dlss5-feed-host.log` | the host: provider name (`"4.1.2"` = ML; `"3.1.6"` = fallback → check the driver), `frame N evaluated`, `FSR GPU X.XX ms/frame` |
| `dlss5-feed.log` | the add-on: builds, `frame N delivered`, the MV provider in use |
| `Documents\SpellForce\Log.txt` | the engine: `resolution : x:… y:…`; the adapter line must mention "(dgVoodoo DX API Layer)" |
| `fsr41-redirect.log` | proxy diversions + hook arming ("hooks: … armed") |
| `ReShade.log` | effect compilation, hooks |

Overlay key: **Home**. Techniques `LUMENITE: Kernel 2.0` and `DLSS 5 Feed`
must both be enabled, with Feed *below* Kernel.

## Tuning

All in `dlss5-feed.cfg` (written by the add-on on first launch), applied after
a game restart:

- `mode` — `2` = neural (normal), `1` = transport self-test (the left half of
  the screen is copied by the host — proof the whole pipe works without any
  upscaler), `0` = off;
- `work_upscale` / `work_resolution` —
  - `0` / `100` — **NativeAA 1:1** (default, recommended): FSR 4.1 enhances
    the backbuffer pixel-for-pixel. Best measured quality on this title;
  - `2` / `50–67` — SR mode: downsample → FSR 4.1 ML upscale back to the
    backbuffer size. **Caveat, learned the hard way:** this only makes sense
    when the game content fills the whole backbuffer. With `centered_ar` and
    factor 1 the game occupies a small centered region of the 4K canvas, the
    downsample destroys the content's real pixels and the image visibly
    smears;
  - `1` + `work_sharpness` — FSR 1 RCAS expand-back sharpening;
- `settle_evals` 1–2 — extra zero-motion dispatches; calms residual shimmer
  on static RTS scenes.

`host64\fsr41.ini`: `sharpness` (RCAS 0–1), `debug_view=1` (FSR debug
visualization), camera near/far.

## Rollback

`deploy\fsr41\uninstall.bat`, then switch Steam back to the default branch.
No original game file is modified; Steam "verify integrity" stays clean
(`config.lua` is left alone — it belongs to the game).

## Credits and licenses

- [jlrouzies-fr/DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) —
  MIT. The 32-bit add-on, the feed shader, the IPC protocol and all the
  plumbing are its code, unchanged.
- [FidelityFX SDK v2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) —
  headers MIT; the signed `amd_fidelityfx_*.dll` under AMD's binary license
  (downloaded at build time, not redistributed here).
- ReShade © Patrick Mours (BSD-3) — fetched from reshade.me at install time.
- [LumeniteFX](https://github.com/umar-afzaal/LumeniteFX),
  [dgVoodoo2](http://dege.freeweb.hu/dgVoodoo2/) © dege.
