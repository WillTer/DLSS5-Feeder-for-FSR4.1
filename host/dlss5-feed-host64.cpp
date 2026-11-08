// dlss5-feed-host64.exe -- the 64-bit helper for the 32-bit add-on, FSR 4.1 edition.
//
// Derived from jlrouzies-fr/DLSS5-Feeder v1.19.1-beta.1 (MIT): the NVIDIA side is
// GONE -- no NGX SDK, no decoy window, no ReShade inside this process, no
// renodx-dlss5 / OptiScaler / Deep-Fried-Chicken consumers, no panel cast. What
// remains is the half that matters: the named pipe, the shared NT-handle
// textures, the two shared fences, and the frame pacing contract -- see
// src/feed_ipc.h, unchanged. Where that host called NGX_CREATE/EVALUATE_DLSS_EXT
// (hoping a neural consumer's detours would intercept), this one calls the
// FidelityFX ffx_api directly: FSR Upscaling 4.1 lives in
// amd_fidelityfx_upscaler_dx12.dll, two files that sit next to this exe.
//
// The add-on spawns this exe under its historical name (host64\dlss5-feed-host64.exe)
// and points the user at host64\dlss5-feed-host.log -- both names are kept so
// the add-on's guidance stays true.
//
// Never-hang contract, inherited verbatim: the game queues a GPU wait on
// fence_out for every frame message it writes; every path here -- failure,
// crash filter, exit -- must end with fence_out signalled past that value.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#include "../src/feed_ipc.h"

#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_upscale.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "version.lib")

#ifndef FEED_BUILD_ID
#define FEED_BUILD_ID "fsr41"
#endif

// ---------------------------------------------------------------------------
// Log -- same file name the add-on tells the user to open.
// ---------------------------------------------------------------------------

static char g_log_path[MAX_PATH];

static void Log(const char *fmt, ...)
{
    static FILE *f = nullptr;
    if (f == nullptr && g_log_path[0] != 0) fopen_s(&f, g_log_path, "a");
    va_list ap;
    va_start(ap, fmt);
    if (f) { vfprintf(f, fmt, ap); fprintf(f, "\n"); fflush(f); }
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fprintf(stdout, "\n");
    fflush(stdout);
}

static void FfxMsg(uint32_t type, const wchar_t *message)
{
    Log("[ffx][%s] %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warn", message);
}

// ---------------------------------------------------------------------------
// Config -- fsr41.ini next to this exe, reread on every start (the add-on
// restarts the host to apply its own knob pushes; this rides the same path).
// ---------------------------------------------------------------------------

static uint64_t g_version_override = 0;   // 0 = newest offered
static bool     g_debug_view       = false;
static float    g_sharpness        = 0.0f;   // 0 = off
static float    g_cam_near = 0.1f, g_cam_far = 1000.0f;

static void ReadIni()
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char *s = strrchr(path, '\\')) strcpy_s(s + 1, MAX_PATH - (s + 1 - path), "fsr41.ini");
    FILE *f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return;
    char line[256];
    while (fgets(line, sizeof line, f))
    {
        char key[64] = {}, val[128] = {};
        if (sscanf_s(line, " %63[^= \t] = %127[^\r\n]", key, 64u, val, 128u) != 2) continue;
        if      (_stricmp(key, "debug_view") == 0) g_debug_view = atoi(val) != 0;
        else if (_stricmp(key, "sharpness") == 0)  g_sharpness = fmaxf(0.0f, fminf(1.0f, float(atof(val))));
        else if (_stricmp(key, "camera_near") == 0) g_cam_near = float(atof(val));
        else if (_stricmp(key, "camera_far")  == 0) g_cam_far  = float(atof(val));
    }
    fclose(f);
    Log("[host] fsr41.ini: debug_view=%d sharpness=%.2f camera %.2f..%.2f%s",
        g_debug_view ? 1 : 0, g_sharpness, g_cam_near, g_cam_far,
        g_version_override ? " (version pinned)" : "");
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Host
{
    ID3D12Device              *dev    = nullptr;
    ID3D12CommandQueue        *queue  = nullptr;
    ID3D12GraphicsCommandList *list   = nullptr;
    static const int           kFrames = 3;
    ID3D12CommandAllocator    *alloc[kFrames] = {};
    UINT64                     alloc_fence[kFrames] = {};
    int                        frame_slot = 0;
    // GPU time of the FSR work, collected one ring later off the internal fence.
    ID3D12QueryHeap           *ts_heap   = nullptr;
    ID3D12Resource            *ts_read   = nullptr;
    UINT64                     ts_freq   = 0;
    bool                       ts_failed = false;
    double                     ts_sum_ms = 0.0;
    unsigned                   ts_n      = 0;
    ID3D12Fence               *fence      = nullptr;   // internal (allocator ring)
    HANDLE                     fence_event = nullptr;
    UINT64                     fence_value = 0;

    // cross-process
    ID3D12Fence *fence_in  = nullptr;   // game signals, host waits
    ID3D12Fence *fence_out = nullptr;   // host signals, game waits

    // FFX
    ffxContext   ffx_ctx = nullptr;
    bool         ffx_ctx_valid = false;
    std::string  ffx_provider;

    ID3D12Resource *tex[FEED_SLOTS] = {};
    UINT            width = 0, height = 0;          // work size (what FSR upscales from)
    UINT            out_width = 0, out_height = 0;  // Output slot: == work (NativeAA) or bigger (SR)
    DXGI_FORMAT     color_fmt = DXGI_FORMAT_UNKNOWN, output_fmt = DXGI_FORMAT_UNKNOWN;
    bool            transport_only = false;
    float           mvsx = 1.0f, mvsy = 1.0f;
    int             sr_quality = -1;                // echoed to the add-on for its label only
};
static Host h;

static ffxFunctions ffx = {};
static HMODULE g_ffx_loader = nullptr;

// ---------------------------------------------------------------------------
// Command submission (allocator ring) -- inherited shape from the DLSS host.
// ---------------------------------------------------------------------------

static void TimingEnsure()
{
    if (h.ts_heap != nullptr || h.ts_failed || h.dev == nullptr) return;
    if (FAILED(h.queue->GetTimestampFrequency(&h.ts_freq)) || h.ts_freq == 0)
    { Log("[host] GPU timing unavailable (no timestamp frequency)"); h.ts_failed = true; return; }
    D3D12_QUERY_HEAP_DESC qd = {};
    qd.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = Host::kFrames * 2;
    if (FAILED(h.dev->CreateQueryHeap(&qd, __uuidof(ID3D12QueryHeap), reinterpret_cast<void **>(&h.ts_heap))))
    { h.ts_failed = true; return; }
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = Host::kFrames * 2 * sizeof(UINT64);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(h.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&h.ts_read))))
    { if (h.ts_heap) { h.ts_heap->Release(); h.ts_heap = nullptr; } h.ts_failed = true; return; }
    Log("[host] GPU timing on (%llu Hz)", static_cast<unsigned long long>(h.ts_freq));
}

static void TimingCollect(int slot)
{
    if (h.ts_read == nullptr) return;
    const size_t base = static_cast<size_t>(slot) * 2;
    D3D12_RANGE want = { base * sizeof(UINT64), (base + 2) * sizeof(UINT64) };
    void *p = nullptr;
    if (FAILED(h.ts_read->Map(0, &want, &p)) || p == nullptr) return;
    const UINT64 *t = static_cast<const UINT64 *>(p);
    if (t[base + 1] > t[base]) { h.ts_sum_ms += 1000.0 * double(t[base + 1] - t[base]) / double(h.ts_freq); ++h.ts_n; }
    const D3D12_RANGE wrote = { 0, 0 };
    h.ts_read->Unmap(0, &wrote);
}

static bool WaitFenceValue(ID3D12Fence *f, UINT64 v, DWORD ms)
{
    if (f == nullptr) return false;
    if (f->GetCompletedValue() >= v) return true;
    const HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr) return false;
    bool ok = false;
    if (SUCCEEDED(f->SetEventOnCompletion(v, ev)))
        ok = WaitForSingleObject(ev, ms) == WAIT_OBJECT_0 && f->GetCompletedValue() >= v;
    CloseHandle(ev);
    return ok;
}

static bool BeginCommands()
{
    if (h.list == nullptr)
    {
        static bool said = false;
        if (!said) { said = true; Log("[host] no command list (a previous fault was not recovered from)"); }
        return false;
    }
    const int slot = h.frame_slot;
    const UINT64 retire = h.alloc_fence[slot];
    if (retire != 0 && h.fence->GetCompletedValue() < retire)
    {
        if (!WaitFenceValue(h.fence, retire, 2000))
        { Log("[host] GPU did not retire allocator slot %d", slot); return false; }
    }
    if (h.alloc_fence[slot] != 0) TimingCollect(slot);
    if (FAILED(h.alloc[slot]->Reset())) return false;
    if (FAILED(h.list->Reset(h.alloc[slot], nullptr))) return false;
    TimingEnsure();
    if (h.ts_heap != nullptr) h.list->EndQuery(h.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    return true;
}

static UINT64 EndCommands()
{
    if (h.ts_heap != nullptr)
    {
        h.list->EndQuery(h.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, h.frame_slot * 2 + 1);
        h.list->ResolveQueryData(h.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, h.frame_slot * 2, 2, h.ts_read,
                                 static_cast<UINT64>(h.frame_slot) * 2 * sizeof(UINT64));
    }
    h.list->Close();
    ID3D12CommandList *lists[] = { h.list };
    h.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++h.fence_value;
    h.queue->Signal(h.fence, v);
    h.alloc_fence[h.frame_slot] = v;
    h.frame_slot = (h.frame_slot + 1) % Host::kFrames;
    return v;
}

static bool g_device_removed = false;

static bool DeviceRemoved(const char *where)
{
    if (h.dev == nullptr) return false;
    if (h.dev->GetDeviceRemovedReason() == S_OK) return false;
    g_device_removed = true;
    Log("[host] device removed (0x%08X) after %s -- exiting", h.dev->GetDeviceRemovedReason(), where);
    return true;
}

// Never execute a list the effect crashed in: release it and try to rebuild.
static void AbortCommands()
{
    if (h.list) { h.list->Release(); h.list = nullptr; }
    if (h.dev)
    {
        ID3D12GraphicsCommandList *l = nullptr;
        if (SUCCEEDED(h.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, h.alloc[h.frame_slot], nullptr,
                                               __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&l))))
        { l->Close(); h.list = l; }
    }
}

// ---------------------------------------------------------------------------
// FFX
// ---------------------------------------------------------------------------

static std::string ProviderName()
{
    if (!h.ffx_ctx_valid) return "(no context)";
    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (ffx.Query(&h.ffx_ctx, &pv.header) != FFX_API_RETURN_OK || pv.versionName == nullptr) return "(query failed)";
    return pv.versionName;
}

// Fills g_version_override from a "4.1.2"-style ini string by matching the
// loader's offered names. Returns the matched id (0 = none / default).
static uint64_t ResolveOverrideByName(const char *want)
{
    if (want == nullptr || want[0] == 0) return 0;
    uint64_t count = 0;
    ffxQueryDescGetVersions q = {};
    q.header.type    = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device         = h.dev;
    q.outputCount    = &count;
    q.versionIds     = nullptr;
    q.versionNames   = nullptr;
    if (ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0) return 0;
    std::vector<uint64_t> ids(count);
    std::vector<const char *> names(count);
    uint64_t cap = count;
    q.outputCount = &cap; q.versionIds = ids.data(); q.versionNames = names.data();
    if (ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK) return 0;
    for (uint64_t i = 0; i < count; ++i)
        if (names[i] && strstr(names[i], want) != nullptr) return ids[i];
    return 0;
}

static bool InitDevice()
{
    IDXGIFactory4 *fact = nullptr;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void **>(&fact))))
    { Log("[host] CreateDXGIFactory2 failed %lu", GetLastError()); return false; }
    IDXGIAdapter1 *amd = nullptr, *any = nullptr;
    for (UINT i = 0; ; ++i)
    {
        IDXGIAdapter1 *a = nullptr;
        if (fact->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d = {};
        a->GetDesc1(&d);
        if (!any && (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) { any = a; a->AddRef(); }
        if (!amd && d.VendorId == 0x1002 && (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) { amd = a; a->AddRef(); }
        a->Release();
    }
    IDXGIAdapter1 *chosen = amd ? amd : any;
    if (!chosen) { Log("[host] no hardware adapter"); fact->Release(); return false; }
    DXGI_ADAPTER_DESC1 dd = {};
    chosen->GetDesc1(&dd);
    char name[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, dd.Description, -1, name, sizeof name, nullptr, nullptr);
    Log("[host] adapter: %s", name);
    const HRESULT hr = D3D12CreateDevice(chosen, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                                         reinterpret_cast<void **>(&h.dev));
    if (amd) amd->Release();
    if (any) any->Release();
    fact->Release();
    if (FAILED(hr) || h.dev == nullptr) { Log("[host] D3D12CreateDevice failed 0x%08X", hr); return false; }

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (FAILED(h.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&h.queue))))
    { Log("[host] CreateCommandQueue failed"); return false; }
    for (int i = 0; i < Host::kFrames; ++i)
        if (FAILED(h.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                 reinterpret_cast<void **>(&h.alloc[i]))))
        { Log("[host] CreateCommandAllocator(%d) failed", i); return false; }
    ID3D12GraphicsCommandList *l = nullptr;
    if (FAILED(h.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, h.alloc[0], nullptr,
                                        __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&l))) ||
        FAILED(l->Close()))
    { Log("[host] CreateCommandList failed"); if (l) l->Release(); return false; }
    h.list = l;
    if (FAILED(h.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&h.fence))))
    { Log("[host] internal fence failed"); return false; }
    h.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

static bool InitFfx()
{
    char dir[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char *s = strrchr(dir, '\\')) *s = 0;
    sprintf_s(path, "%s\\amd_fidelityfx_loader_dx12.dll", dir);
    g_ffx_loader = LoadLibraryA(path);
    if (g_ffx_loader == nullptr) { Log("[host] cannot load amd_fidelityfx_loader_dx12.dll (error %lu) -- it belongs next to this exe", GetLastError()); return false; }
    ffxLoadFunctions(&ffx, g_ffx_loader);
    if (!ffx.CreateContext || !ffx.DestroyContext || !ffx.Query || !ffx.Dispatch)
    { Log("[host] loader exports missing"); return false; }

    uint64_t count = 0;
    ffxQueryDescGetVersions q = {};
    q.header.type    = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device         = h.dev;
    q.outputCount    = &count;
    q.versionIds     = nullptr;
    q.versionNames   = nullptr;
    if (ffx.Query(nullptr, &q.header) == FFX_API_RETURN_OK && count > 0)
    {
        std::vector<uint64_t> ids(count);
        std::vector<const char *> names(count);
        uint64_t cap = count;
        q.outputCount = &cap; q.versionIds = ids.data(); q.versionNames = names.data();
        if (ffx.Query(nullptr, &q.header) == FFX_API_RETURN_OK)
            for (uint64_t i = 0; i < count; ++i)
                Log("[host] upscaler version offered: \"%s\" (id 0x%016llX%s)",
                    names[i] ? names[i] : "?", static_cast<unsigned long long>(ids[i]),
                    (ids[i] & (1ull << 31)) ? ", driver" : "");
    }
    else
        Log("[host] version enumeration unavailable (loader cannot see amd_fidelityfx_upscaler_dx12.dll?)");
    return true;
}

static bool CreateUpscaleContext(UINT w, UINT h_, UINT out_w, UINT out_h, int depth_inverted, int hdr)
{
    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device      = h.dev;

    ffxCreateContextDescUpscaleVersion ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version     = FFX_UPSCALER_VERSION;

    ffxCreateContextDescUpscale ups = {};
    ups.header.type   = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    ups.flags         = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    if (depth_inverted) ups.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (hdr)            ups.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    ups.maxRenderSize  = { w, h_ };
    ups.maxUpscaleSize = { out_w, out_h };
    ups.fpMessage      = &FfxMsg;
    ups.header.pNext   = &ver.header;
    ver.header.pNext   = &backend.header;

    ffxOverrideVersion ov = {};
    if (g_version_override)
    {
        ov.header.type       = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        ov.versionId         = g_version_override;
        backend.header.pNext = &ov.header;
    }

    ffxContext ctx = nullptr;
    const ffxReturnCode_t rc = ffx.CreateContext(&ctx, &ups.header, nullptr);
    if (rc != FFX_API_RETURN_OK || ctx == nullptr)
    { Log("[host] FFX CreateContext failed rc=%u (%ux%u -> %ux%u)", rc, w, h_, out_w, out_h); return false; }
    h.ffx_ctx = ctx;
    h.ffx_ctx_valid = true;
    h.ffx_provider = ProviderName();
    Log("[host] FFX context up: provider \"%s\" (render %ux%u -> upscale %ux%u)",
        h.ffx_provider.c_str(), w, h_, out_w, out_h);
    if (h.ffx_provider.find("4.1") == std::string::npos && !g_version_override)
        Log("[host] NOTE: this is not 4.1 -- the GPU/driver offered an older upscaler; FSR 4.1 on RX 6000 needs Adrenalin 26.10.1+");
    return true;
}

static void DestroyUpscaleContext()
{
    if (!h.ffx_ctx_valid) return;
    // Drain first: destroying under in-flight dispatch work hung GPUs before.
    WaitFenceValue(h.fence, h.fence_value, 2000);
    ffx.DestroyContext(&h.ffx_ctx, nullptr);
    h.ffx_ctx = nullptr;
    h.ffx_ctx_valid = false;
}

// The quality id the add-on prints next to its SR label (its own 0..4 scale:
// 0 Performance .. 4 Ultra Quality -- display only, nothing derives from it).
static int PickSrQuality(UINT w, UINT h_, UINT out_w, UINT out_h)
{
    const double r = fmax(double(out_w) / fmax(1.0, double(w)), double(out_h) / fmax(1.0, double(h_)));
    if      (r <= 1.20) return 4;   // "Ultra Quality"     (work 100->? small ratios)
    else if (r <= 1.50) return 2;   // "Quality"
    else if (r <= 1.70) return 1;   // "Balanced"
    else if (r <= 2.00) return 0;   // "Performance"
    else if (r <= 3.00) return 3;   // "Ultra Performance"
    return -1;                      // beyond any preset
}

// SEH guard around the dispatch: the effect DLL runs in this process, and a
// driver crash inside it must not kill the game's fence contract.
static ffxReturnCode_t SafeDispatch(const ffxDispatchDescUpscale *d, DWORD *code)
{
    __try
    {
        return ffx.Dispatch(&h.ffx_ctx, &d->header);
    }
    __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        return FFX_API_RETURN_ERROR;
    }
}

static bool Evaluate(ID3D12Resource *color, ID3D12Resource *output, ID3D12Resource *depth, ID3D12Resource *mv,
                     UINT w, UINT h_, int reset, float mvsx, float mvsy, float jitter_x, float jitter_y,
                     int settle, float frame_dt_ms)
{
    if (!h.ffx_ctx_valid) return false;
    if (!BeginCommands()) return false;
    if (settle > 8) settle = 8;

    ffxDispatchDescUpscale d = {};
    d.header.type       = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList       = h.list;
    d.color             = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth             = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors     = ffxApiGetResourceDX12(mv,     FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output            = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset      = { jitter_x, jitter_y };
    d.motionVectorScale = { mvsx, mvsy };         // feeder MVs are pixel-space at scale 1
    d.renderSize        = { w, h_ };
    d.upscaleSize       = { h.out_width, h.out_height };
    d.enableSharpening  = g_sharpness > 0.0f;
    d.sharpness         = g_sharpness;
    d.frameTimeDelta    = frame_dt_ms > 0.0f ? frame_dt_ms : 16.6f;
    d.preExposure       = 1.0f;
    d.reset             = reset != 0;
    d.cameraNear        = g_cam_near;
    d.cameraFar         = g_cam_far;
    d.cameraFovAngleVertical = 1.0f;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags             = g_debug_view ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0u;

    DWORD ecode = 0;
    ffxReturnCode_t re = SafeDispatch(&d, &ecode);
    if (ecode != 0)
    { AbortCommands(); Log("[host] FFX dispatch raised 0x%08lX", ecode); return false; }

    // settle_evals: the same frame again with zero motion, so the upscaler's
    // history settles on a still image (the add-on's slider, v11).
    for (int i = 0; i < settle && re == FFX_API_RETURN_OK; ++i)
    {
        D3D12_RESOURCE_BARRIER uav = {};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = output;
        h.list->ResourceBarrier(1, &uav);
        d.reset = false;
        d.motionVectorScale = { 0.0f, 0.0f };
        re = SafeDispatch(&d, &ecode);
        if (ecode != 0) { AbortCommands(); Log("[host] FFX settle dispatch raised 0x%08lX", ecode); return false; }
    }

    EndCommands();
    if (re != FFX_API_RETURN_OK)
    { Log("[host] FFX dispatch failed rc=%u", re); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Pipe framing
// ---------------------------------------------------------------------------

static bool ReadFull(HANDLE pipe, void *buf, DWORD len)
{
    uint8_t *p = static_cast<uint8_t *>(buf);
    while (len > 0)
    {
        DWORD got = 0;
        if (!ReadFile(pipe, p, len, &got, nullptr) || got == 0) return false;
        p += got; len -= got;
    }
    return true;
}

static bool WriteFull(HANDLE pipe, const void *buf, DWORD len)
{
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    while (len > 0)
    {
        DWORD put = 0;
        if (!WriteFile(pipe, p, len, &put, nullptr) || put == 0) return false;
        p += put; len -= put;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Serve
// ---------------------------------------------------------------------------

static int Serve(DWORD game_pid)
{
    char name[128];
    sprintf_s(name, FEED_PIPE_FMT, static_cast<unsigned long>(game_pid));
    // Same pacing rationale as the DLSS host: a small IN buffer bounds the
    // client's run-ahead to ~12 frames instead of ~48, so burst-then-collapse
    // rhythms cannot form (its issue #15).
    HANDLE pipe = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   1, 1024, 256, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { Log("[host] CreateNamedPipe failed %lu", GetLastError()); return 1; }
    Log("[host] serving on %s", name);
    {
        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED)
        { Log("[host] ConnectNamedPipe failed %lu", GetLastError()); CloseHandle(pipe); return 1; }
    }

    FeedHello hello = {};
    if (!ReadFull(pipe, &hello, FEED_HELLO_V1_SIZE) || hello.magic != FEED_IPC_MAGIC)
    { Log("[host] bad hello"); CloseHandle(pipe); return 1; }
    if (hello.version >= 2 && !ReadFull(pipe, &hello.client_kind, sizeof(hello.client_kind)))
    { Log("[host] truncated hello"); CloseHandle(pipe); return 1; }
    if (hello.version >= 4 && !ReadFull(pipe, &hello.self_process, sizeof(hello.self_process)))
    { Log("[host] truncated hello (v4 self_process)"); CloseHandle(pipe); return 1; }

    FeedHelloAck ack = { FEED_IPC_MAGIC, FEED_IPC_VERSION, 0, 0 };   // no panel in this host
    WriteFull(pipe, &ack, sizeof(ack));
    if (hello.version != FEED_IPC_VERSION)
    {
        Log("[host] the game add-on speaks protocol v%u, this host v%u -- reinstall the add-on and host64\\ together",
            hello.version, FEED_IPC_VERSION);
        CloseHandle(pipe);
        return 1;
    }
    if (hello.client_kind != FEED_CLIENT_D3D11)
    {
        Log("[host] this FSR 4.1 host supports D3D11 clients only (got kind %u -- GL/Vulkan need the DLSS host)",
            hello.client_kind);
        CloseHandle(pipe);
        return 1;
    }
    Log("[host] game pid %u connected (protocol v%u, D3D11 client -- the game creates the shared textures)",
        hello.pid, hello.version);

    HANDLE hgame = nullptr;
    if (hello.version >= 4 && hello.self_process != 0)
    {
        HANDLE h2 = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(hello.self_process));
        if (GetProcessId(h2) == hello.pid) hgame = h2;
    }
    if (hgame == nullptr)
        hgame = OpenProcess(PROCESS_DUP_HANDLE, FALSE, hello.pid);
    if (hgame == nullptr)
    { Log("[host] no process handle for pid %u (error %lu)", hello.pid, GetLastError()); CloseHandle(pipe); return 1; }

    HANDLE hin = nullptr, hout = nullptr;
    h.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&h.fence_in));
    h.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&h.fence_out));
    if (h.fence_in == nullptr || h.fence_out == nullptr ||
        FAILED(h.dev->CreateSharedHandle(h.fence_in, nullptr, GENERIC_ALL, nullptr, &hin)) ||
        FAILED(h.dev->CreateSharedHandle(h.fence_out, nullptr, GENERIC_ALL, nullptr, &hout)))
    { Log("[host] shared fence creation failed"); CloseHandle(pipe); return 1; }
    HANDLE game_in = nullptr, game_out = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), hin, hgame, &game_in, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(GetCurrentProcess(), hout, hgame, &game_out, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        Log("[host] could not duplicate the shared fences into the game (error %lu)", GetLastError());
        CloseHandle(pipe);
        return 1;
    }

    LARGE_INTEGER qpf, qprev = {};
    QueryPerformanceFrequency(&qpf);
    UINT64 evaluated = 0;
    int     build_fails = 0;

    for (;;)
    {
        BYTE tag = 0;
        DWORD got = 0;
        if (!ReadFile(pipe, &tag, 1, &got, nullptr) || got != 1)
        { Log("[host] pipe closed by the game"); break; }

        if (tag == 'B')
        {
            FeedBuild b = {};
            if (!ReadFull(pipe, &b, sizeof(b))) break;
            Log("[host] build: work %ux%u -> output %ux%u color=%u output=%u hdr=%d inverted=%d transport=%d",
                b.width, b.height, b.target_width ? b.target_width : b.width,
                b.target_height ? b.target_height : b.height,
                b.color_fmt, b.output_fmt, b.hdr, b.depth_inverted, b.transport);

            if ((b.client_flags & FEED_BUILD_HOST_CREATES) != 0)
            {
                Log("[host] this game's D3D11 device asked the host to create the shared set -- not supported "
                    "by the FSR 4.1 host yet (a feature-level 10 device?); refusing the build");
                FeedBuildAck back = {};
                back.ok = 0;
                back.fence_in  = reinterpret_cast<uint64_t>(game_in);
                back.fence_out = reinterpret_cast<uint64_t>(game_out);
                WriteFull(pipe, &back, sizeof(back));
                continue;
            }

            // Drain, then tear down the old set (same ordering as the DLSS host:
            // release under in-flight work hung GPUs).
            if (!WaitFenceValue(h.fence, h.fence_value, 2000))
                Log("[host] rebuild: the previous frame's GPU work did not retire within 2 s");
            DestroyUpscaleContext();
            for (int i = 0; i < FEED_SLOTS; ++i)
                if (h.tex[i]) { h.tex[i]->Release(); h.tex[i] = nullptr; }

            const bool want_sr = b.target_width != 0 && b.target_height != 0 &&
                                 (b.target_width != b.width || b.target_height != b.height) && b.transport == 0;
            const UINT out_w = want_sr ? b.target_width : b.width;
            const UINT out_h = want_sr ? b.target_height : b.height;
            h.transport_only = b.transport != 0;
            h.mvsx = b.mv_scale_x; h.mvsy = b.mv_scale_y;

            bool ok = true;
            int sr_quality = -1;
            bool sr_unavailable = false;
            if (want_sr)
            {
                sr_quality = PickSrQuality(b.width, b.height, out_w, out_h);
                if (sr_quality < 0)
                {
                    Log("[host] work_upscale=2: no preset covers %ux%u -> %ux%u; telling the game to rebuild 1:1",
                        b.width, b.height, out_w, out_h);
                    sr_unavailable = true;
                    ok = false;
                }
            }

            // Open the game's four textures (duplicate the handles out).
            for (int i = 0; i < FEED_SLOTS && ok; ++i)
            {
                HANDLE local = nullptr;
                if (!DuplicateHandle(hgame, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(b.tex[i])),
                                     GetCurrentProcess(), &local, 0, FALSE, DUPLICATE_SAME_ACCESS))
                { Log("[host] DuplicateHandle(tex %d) failed %lu", i, GetLastError()); ok = false; break; }
                const HRESULT hr = h.dev->OpenSharedHandle(local, __uuidof(ID3D12Resource),
                                                           reinterpret_cast<void **>(&h.tex[i]));
                CloseHandle(local);
                if (FAILED(hr)) { Log("[host] OpenSharedHandle(tex %d) failed 0x%08X", i, hr); ok = false; continue; }

                const D3D12_RESOURCE_DESC td = h.tex[i]->GetDesc();
                const UINT want_w = (i == FEED_OUTPUT) ? out_w : b.width;
                const UINT want_h = (i == FEED_OUTPUT) ? out_h : b.height;
                const DXGI_FORMAT want_fmt =
                    i == FEED_COLOR  ? static_cast<DXGI_FORMAT>(b.color_fmt) :
                    i == FEED_OUTPUT ? static_cast<DXGI_FORMAT>(b.output_fmt) :
                    i == FEED_DEPTH  ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16_FLOAT;
                const bool needs_uav = (i == FEED_OUTPUT) && !h.transport_only;
                if (td.Width != static_cast<UINT64>(want_w) || td.Height != want_h || td.Format != want_fmt ||
                    (needs_uav && (td.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0))
                {
                    Log("[host] the game's %s texture is %ux%u fmt=%u flags=0x%X, not %ux%u fmt=%u%s -- refusing the build",
                        FeedSlotName(i), static_cast<unsigned>(td.Width), td.Height, td.Format, td.Flags,
                        want_w, want_h, want_fmt, needs_uav ? " with a UAV" : "");
                    ok = false;
                }
            }

            ffxReturnCode_t rc = FFX_API_RETURN_OK;
            if (ok && !h.transport_only)
            {
                if (b.flags_override >= 0)
                    Log("[host] flags_override=%d carried over from NGX semantics; the FFX flags are derived from the build message instead",
                        b.flags_override);
                ok = CreateUpscaleContext(b.width, b.height, out_w, out_h, b.depth_inverted, b.hdr);
                if (!ok && ++build_fails == 1)
                    Log("[host] the FFX create failed once; the add-on will retry with a fresh host if it does not recover");
                rc = ok ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR;
            }
            if (ok)
            {
                h.width = b.width; h.height = b.height;
                h.out_width = out_w; h.out_height = out_h;
                h.color_fmt  = static_cast<DXGI_FORMAT>(b.color_fmt);
                h.output_fmt = static_cast<DXGI_FORMAT>(b.output_fmt);
                h.sr_quality = sr_quality;
                if (h.transport_only)
                    Log("[host] transport-only mode: Color's left half will be copied to Output, no upscaler");
            }
            evaluated = 0;

            FeedBuildAck back = {};
            back.ok         = ok ? 1 : 0;
            back.ngx_result = static_cast<uint32_t>(rc);   // ffx return code, 0 = OK
            if (sr_unavailable)  back.flags |= FEED_ACK_SR_UNAVAILABLE;
            else if (ok && want_sr) { back.flags |= FEED_ACK_SR_ACTIVE; back.sr_quality = static_cast<uint32_t>(sr_quality); }
            back.fence_in   = reinterpret_cast<uint64_t>(game_in);
            back.fence_out  = reinterpret_cast<uint64_t>(game_out);
            back.output_fmt = b.output_fmt;
            WriteFull(pipe, &back, sizeof(back));
            if (!ok && DeviceRemoved("a rebuild")) break;
        }
        else if (tag == 'W')
        {
            FeedWindowMsg wm = {};
            if (!ReadFull(pipe, &wm, sizeof(wm))) break;
            static bool said = false;
            if (!said) { said = true; Log("[host] 'W' window resize (%ux%u) ignored -- this host has no window", wm.width, wm.height); }
        }
        else if (tag == 'C')
        {
            FeedCastMsg cm = {};
            if (!ReadFull(pipe, &cm, sizeof(cm))) break;
            static bool said = false;
            if (!said && cm.shown) { said = true; Log("[host] 'C' cast state ignored -- no panel in the FSR 4.1 host"); }
        }
        else if (tag == 'O')
        {
            static bool said = false;
            if (!said) { said = true; Log("[host] 'O' overlay toggle ignored -- no ReShade in the FSR 4.1 host"); }
        }
        else if (tag == 'F')
        {
            FeedFrameMsg fm = {};
            if (!ReadFull(pipe, &fm, sizeof(fm))) break;

            // No context yet (create failed, transport off): release the wait
            // and keep the session alive -- same shape as the DLSS host.
            if (!h.ffx_ctx_valid && !h.transport_only)
            {
                h.fence_out->Signal(fm.n);
                continue;
            }

            // Order the dispatch behind the game's input copies on the GPU
            // timeline; the CPU does not wait on fence_in (issue #15 lesson).
            h.queue->Wait(h.fence_in, fm.n);

            double dt_ms = 16.6;
            {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                if (qprev.QuadPart != 0)
                    dt_ms = 1000.0 * double(now.QuadPart - qprev.QuadPart) / double(qpf.QuadPart);
                qprev = now;
            }

            bool done = false;
            if (h.transport_only)
            {
                if (BeginCommands())
                {
                    // Left half only: a split screen is unambiguous proof the
                    // host's output reaches the display.
                    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
                    src.pResource = h.tex[FEED_COLOR]; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst.pResource = h.tex[FEED_OUTPUT]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    D3D12_BOX box = { 0, 0, 0, h.width / 2, h.height, 1 };
                    h.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                    EndCommands();
                    done = true;
                }
            }
            else
            {
                static uint32_t settle_seen = ~0u;
                static float hold_seen = -1.0f;
                if (fm.settle_evals != settle_seen)
                { settle_seen = fm.settle_evals; Log("[host] settle: %u extra zero-motion dispatch(es) per frame", fm.settle_evals); }
                if (fm.hold_strength != hold_seen)
                { hold_seen = fm.hold_strength; if (fm.hold_strength > 0.0f) Log("[host] hold_strength %.2f requested -- the FSR 4.1 host ignores the stabiliser (use settle instead)", fm.hold_strength); }
                done = Evaluate(h.tex[FEED_COLOR], h.tex[FEED_OUTPUT], h.tex[FEED_DEPTH], h.tex[FEED_MV],
                                h.width, h.height, fm.reset ? 1 : 0, h.mvsx, h.mvsy, fm.jitter_x, fm.jitter_y,
                                static_cast<int>(fm.settle_evals), static_cast<float>(dt_ms));
            }

            if (done)
            {
                h.queue->Signal(h.fence_out, fm.n);
                ++evaluated;
            }
            else
            {
                h.fence_out->Signal(fm.n);     // CPU-signal so the game never hangs on us
                if (DeviceRemoved("an evaluate")) break;
            }

            if (fm.n <= 3 || (fm.n % 1800) == 0)
            {
                char gpu_part[64] = "";
                if (h.ts_n > 0)
                {
                    sprintf_s(gpu_part, ", FSR GPU %.2f ms/frame", h.ts_sum_ms / double(h.ts_n));
                    h.ts_sum_ms = 0.0; h.ts_n = 0;
                }
                Log("[host] frame %llu evaluated%s", static_cast<unsigned long long>(fm.n), gpu_part);
            }
        }
        else
        {
            Log("[host] unknown tag 0x%02X", tag);
            break;
        }
    }

    // The game may still hold GPU waits on fence_out for messages it wrote but
    // we never read: release them all before dying (inherited, load-bearing).
    if (!g_device_removed) WaitFenceValue(h.fence, h.fence_value, 2000);
    if (h.fence_out != nullptr) h.fence_out->Signal(UINT64_MAX);
    Log("[host] pending game fence waits released; exiting%s", g_device_removed ? " (device removed)" : "");
    CloseHandle(pipe);
    return g_device_removed ? 3 : 0;
}

// ---------------------------------------------------------------------------
// --test: stand alone, prove the whole FFX path without a game.
// ---------------------------------------------------------------------------

static int RunTest()
{
    const UINT w = 640, ht = 384;
    if (!CreateUpscaleContext(w, ht, w, ht, 0, 0)) return 3;
    ID3D12Resource *color = nullptr, *depth = nullptr, *mv = nullptr, *output = nullptr;
    auto mk = [&](UINT ww, UINT hh, DXGI_FORMAT fmt, bool uav) -> ID3D12Resource * {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = ww; rd.Height = hh; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = fmt; rd.SampleDesc.Count = 1;
        if (uav) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource *r = nullptr;
        h.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                       uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                       nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&r));
        return r;
    };
    color  = mk(w, ht, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    depth  = mk(w, ht, DXGI_FORMAT_R32_FLOAT, false);
    mv     = mk(w, ht, DXGI_FORMAT_R16G16_FLOAT, false);
    output = mk(w, ht, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    if (!color || !depth || !mv || !output) { Log("[host] test textures failed"); return 3; }
    UINT64 good = 0;
    for (int i = 0; i < 120; ++i)
        if (Evaluate(color, output, depth, mv, w, ht, i == 0 ? 1 : 0, 1.0f, 1.0f, 0.0f, 0.0f, 0, 16.6f))
            ++good;
    Log("[host] --test: %llu/120 dispatches ok, provider \"%s\"", static_cast<unsigned long long>(good), h.ffx_provider.c_str());
    if (color) color->Release();
    if (depth) depth->Release();
    if (mv) mv->Release();
    if (output) output->Release();
    DestroyUpscaleContext();
    return good == 120 ? 0 : 3;
}

// ---------------------------------------------------------------------------

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep)
{
    Log("[host] crashed: exception 0x%08lX at %p -- releasing the game's fence waits and exiting",
        ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    if (h.fence_out != nullptr) h.fence_out->Signal(UINT64_MAX);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        MessageBoxA(nullptr,
                    "This is the 64-bit FSR 4.1 helper for the Feeder ReShade add-on.\r\n\r\n"
                    "It is started by the add-on from inside the game -- there is nothing to run here.\r\n"
                    "Its log is dlss5-feed-host.log, next to this file.",
                    "FSR 4.1 Feed helper", MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    GetModuleFileNameA(nullptr, g_log_path, MAX_PATH);
    if (char *s = strrchr(g_log_path, '\\'))
        strcpy_s(s + 1, MAX_PATH - (s + 1 - g_log_path), "dlss5-feed-host.log");
    // Not writable (an install under Program Files)? Fall back to LocalAppData.
    {
        FILE *f = nullptr;
        if (fopen_s(&f, g_log_path, "w") == 0 && f) fclose(f);
        else
        {
            char fallback[MAX_PATH] = {};
            size_t n = 0;
            if (getenv_s(&n, fallback, MAX_PATH, "LOCALAPPDATA") == 0 && n > 1)
            {
                strcat_s(fallback, MAX_PATH, "\\FSR41-Feeder");
                CreateDirectoryA(fallback, nullptr);
                strcat_s(fallback, MAX_PATH, "\\dlss5-feed-host.log");
                FILE *g = nullptr;
                if (fopen_s(&g, fallback, "w") == 0 && g) { fclose(g); strcpy_s(g_log_path, MAX_PATH, fallback); }
            }
        }
    }
    SetUnhandledExceptionFilter(&CrashFilter);

    Log("dlss5-feed-host64 (FSR 4.1 edition) commit %s (built %s %s)", FEED_BUILD_ID, __DATE__, __TIME__);
    Log("[host] command line (argc=%d): %s", argc, GetCommandLineA());
    Log("[host] log file: %s", g_log_path);

    timeBeginPeriod(1);   // every Sleep here is a frame-pacing decision

    bool test = false;
    DWORD pid = 0;
    for (int i = 1; i < argc; ++i)
    {
        if      (strcmp(argv[i], "--test") == 0) test = true;
        else if (strcmp(argv[i], "--hide") == 0) {}            // no window exists to hide
        else if (strcmp(argv[i], "--behind") == 0) {}
        else if (strcmp(argv[i], "--gpu-priority") == 0) Log("[host] --gpu-priority ignored in the FSR 4.1 host");
        else if (pid == 0 && (pid = static_cast<DWORD>(strtoul(argv[i], nullptr, 10))) != 0) {}
        else Log("[host] ignoring an argument I do not understand: %s", argv[i]);
    }
    if (!test && pid == 0)
    {
        Log("usage: dlss5-feed-host64 --test | dlss5-feed-host64 <game pid> [--hide]");
        return 1;
    }

    if (!InitDevice())  return 1;
    if (!InitFfx())     return 1;
    ReadIni();
    {
        // --pin <name> was consumed above as unknown; resolve a pinned version here
        char want[32] = {};
        size_t n = 0;
        if (getenv_s(&n, want, sizeof want, "FSR41_VERSION") == 0 && n > 0)
            if (uint64_t id = ResolveOverrideByName(want)) { g_version_override = id; Log("[host] FSR41_VERSION pins upscaler \"%s\" (id 0x%016llX)", want, static_cast<unsigned long long>(id)); }
    }

    const int rc = test ? RunTest() : Serve(pid);
    Log("[host] exit %d", rc);
    return rc;
}
