// ffx-smoke -- proof that FidelityFX FSR 4.1 (the ML upscaler) can be created and
// dispatched from a plain 64-bit process on this machine's GPU, before the feeder
// host stakes its architecture on it.
//
// What it does, in order:
//   1. picks the AMD adapter (falls back to the default one) and prints it;
//   2. loads amd_fidelityfx_loader_dx12.dll and enumerates the upscaler versions
//      the loader offers on THIS device -- "FSR4.1.2" in the list is the whole
//      point of this test on an RX 6000 (RDNA 2) board;
//   3. creates an Upscale context at FFX_UPSCALER_VERSION and asks the context
//      which provider version it actually got (an unsupported GPU silently gets
//      FSR 3.1.6 instead -- the query is how you tell);
//   4. feeds it a synthetic frame (gradient + circles + thin lines, a depth ramp,
//      zero motion) at 1920x1080 in NativeAA 1:1, the feeder's mode;
//   5. times 60 dispatches wall-clock (submit..fence-wait), dumps input and
//      output as .bmp next to the exe, and optionally re-runs the loop pinned to
//      the FSR3 version for an A/B of the RDNA 2 shader-inference cost (--ab).
//
// Exit code: 0 only when the provider version string names 4.1 and every
// dispatch returned FFX_API_RETURN_OK.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>

#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_upscale.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// ---------------------------------------------------------------- logging

static FILE *g_log = nullptr;

static void Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fprintf(stdout, "\n");
    fflush(stdout);
    if (g_log)
    {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fprintf(g_log, "\n");
        fflush(g_log);
    }
}

static void FfxMsg(uint32_t type, const wchar_t *message)
{
    Log("[ffx][%s] %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warn", message);
}

// ---------------------------------------------------------------- f16

static uint16_t F32ToF16(float f)
{
    uint32_t x; memcpy(&x, &f, sizeof x);
    const uint16_t sign = static_cast<uint16_t>((x >> 16) & 0x8000u);
    int32_t      exp    = static_cast<int32_t>((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t     man    = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFFu) == 0xFFu) return sign | 0x7C00u;           // inf/nan
    if (exp <= 0)   return sign;                                        // flush to zero
    if (exp >= 31)  return sign | 0x7BFFu;                              // saturate
    return static_cast<uint16_t>(sign | (exp << 10) | (man >> 13));
}

// ---------------------------------------------------------------- d3d12 boilerplate

struct Gpu
{
    ID3D12Device             *dev    = nullptr;
    ID3D12CommandQueue       *queue  = nullptr;
    ID3D12CommandAllocator   *alloc  = nullptr;
    ID3D12GraphicsCommandList *list  = nullptr;
    ID3D12Fence              *fence  = nullptr;
    HANDLE                    event  = nullptr;
    UINT64                    value  = 0;
    std::wstring              adapter_name;
};

static bool WaitGpu(Gpu &g)
{
    if (g.queue->Signal(g.fence, ++g.value) != S_OK) return false;
    if (g.fence->SetEventOnCompletion(g.value, g.event) != S_OK) return false;
    return WaitForSingleObject(g.event, 10000) == WAIT_OBJECT_0;
}

static ID3D12Resource *MakeTex(Gpu &g, UINT w, UINT h, DXGI_FORMAT fmt, bool uav, D3D12_RESOURCE_STATES initial)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    if (uav) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *r = nullptr;
    if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initial, nullptr,
                                              __uuidof(ID3D12Resource), reinterpret_cast<void **>(&r))))
        return nullptr;
    return r;
}

// Upload rows of raw texel bytes into a DEFAULT-heap texture created in COPY_DEST.
template <typename Fill>
static bool UploadTex(Gpu &g, ID3D12Resource *dst, UINT w, UINT h, UINT bpp, Fill fill)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    const UINT64 row    = static_cast<UINT64>(w) * bpp;
    rd.Width            = row * h;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *up = nullptr;
    if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&up))) || !up)
        return false;
    uint8_t *p = nullptr;
    if (FAILED(up->Map(0, nullptr, reinterpret_cast<void **>(&p)))) { up->Release(); return false; }
    for (UINT y = 0; y < h; ++y, p += row) fill(p, y);
    up->Unmap(0, nullptr);

    if (FAILED(g.alloc->Reset())) { up->Release(); return false; }
    if (FAILED(g.list->Reset(g.alloc, nullptr))) { up->Release(); return false; }
    D3D12_TEXTURE_COPY_LOCATION src = {}, dd = {};
    src.pResource = up;  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format   = dst->GetDesc().Format;
    src.PlacedFootprint.Footprint.Width    = w;
    src.PlacedFootprint.Footprint.Height   = h;
    src.PlacedFootprint.Footprint.Depth    = 1;
    src.PlacedFootprint.Footprint.RowPitch = row;
    dd.pResource = dst;  dd.Type  = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dd, 0, 0, 0, &src, nullptr);
    g.list->Close();
    ID3D12CommandList *lists[] = { g.list };
    g.queue->ExecuteCommandLists(1, lists);
    const bool ok = WaitGpu(g);
    up->Release();
    return ok;
}

// ---------------------------------------------------------------- bmp writer (24-bit)

static bool WriteBMP24(const char *path, const uint8_t *bgr_bottom_up_rows, UINT w, UINT h)
{
    const UINT row24 = ((24u * w + 31u) / 32u) * 4u;   // padded to 4 bytes
    const uint64_t data = static_cast<uint64_t>(row24) * h;
    FILE *f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    uint8_t fh[54] = {};
    fh[0] = 'B'; fh[1] = 'M';
    const uint32_t total = static_cast<uint32_t>(54 + data);
    memcpy(fh + 2, &total, 4);
    const uint32_t off = 54;
    memcpy(fh + 10, &off, 4);
    const uint32_t sz = 40;
    memcpy(fh + 14, &sz, 4);
    memcpy(fh + 18, &w, 4);
    memcpy(fh + 22, &h, 4);
    const uint16_t planes = 1, bpp = 24;
    memcpy(fh + 26, &planes, 2);
    memcpy(fh + 28, &bpp, 2);
    fwrite(fh, 1, 54, f);
    std::vector<uint8_t> row(row24, 0);
    for (UINT i = 0; i < h; ++i)
    {
        const uint8_t *srcrow = bgr_bottom_up_rows + static_cast<size_t>(i) * 3 * w;   // caller gives bottom-up
        memcpy(row.data(), srcrow, 3 * w);
        fwrite(row.data(), 1, row24, f);
    }
    fclose(f);
    return true;
}

// ---------------------------------------------------------------- the test

static const UINT kW = 1920, kH = 1080;

struct VersionInfo { std::vector<uint64_t> ids; std::vector<std::string> names; };

static VersionInfo QueryVersions(ffxFunctions &ffx, ID3D12Device *dev)
{
    VersionInfo out;
    uint64_t count = 0;
    ffxQueryDescGetVersions q = {};
    q.header.type      = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType   = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device           = dev;
    q.outputCount      = &count;
    q.versionIds       = nullptr;
    q.versionNames     = nullptr;
    if (ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0) return out;
    out.ids.resize(count);
    out.names.resize(count);
    std::vector<const char *> names(count);
    uint64_t cap = count;
    q.outputCount  = &cap;
    q.versionIds   = out.ids.data();
    q.versionNames = names.data();
    if (ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK) { out.ids.clear(); out.names.clear(); return out; }
    for (uint64_t i = 0; i < count; ++i) out.names[i] = names[i];
    return out;
}

static ffxContext CreateUpscale(ffxFunctions &ffx, ID3D12Device *dev, uint64_t override_id, ffxReturnCode_t *rc)
{
    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device      = dev;

    ffxCreateContextDescUpscaleVersion ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version     = FFX_UPSCALER_VERSION;

    ffxCreateContextDescUpscale ups = {};
    ups.header.type     = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    ups.flags           = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    ups.maxRenderSize   = { kW, kH };
    ups.maxUpscaleSize  = { kW, kH };
    ups.fpMessage       = &FfxMsg;
    ups.header.pNext    = &ver.header;
    ver.header.pNext    = &backend.header;

    ffxOverrideVersion ov = {};
    if (override_id)
    {
        ov.header.type  = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        ov.versionId    = override_id;
        backend.header.pNext = &ov.header;
    }

    ffxContext ctx = nullptr;
    *rc = ffx.CreateContext(&ctx, &ups.header, nullptr);
    return ctx;
}

static std::string ProviderName(ffxFunctions &ffx, ffxContext ctx)
{
    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (ffx.Query(&ctx, &pv.header) != FFX_API_RETURN_OK || pv.versionName == nullptr) return "(query failed)";
    return pv.versionName;
}

static double RunFrames(ffxFunctions &ffx, Gpu &g, ffxContext ctx,
                        ID3D12Resource *color, ID3D12Resource *depth, ID3D12Resource *mv,
                        ID3D12Resource *output, int frames)
{
    LARGE_INTEGER freq, t0;
    QueryPerformanceFrequency(&freq);
    std::vector<double> ms;
    for (int i = 0; i < frames; ++i)
    {
        if (FAILED(g.alloc->Reset()) || FAILED(g.list->Reset(g.alloc, nullptr))) return -1.0;
        ffxDispatchDescUpscale d = {};
        d.header.type      = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList      = g.list;
        d.color            = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth            = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors    = ffxApiGetResourceDX12(mv,     FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output           = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset     = { 0.0f, 0.0f };
        d.motionVectorScale = { 1.0f, 1.0f };     // pixel-space, and ours are zero anyway
        d.renderSize       = { kW, kH };
        d.upscaleSize      = { kW, kH };          // NativeAA 1:1 -- the feeder's mode
        d.enableSharpening = false;
        d.frameTimeDelta   = 16.6f;
        d.preExposure      = 1.0f;
        d.reset            = i == 0;
        d.cameraNear       = 0.1f;
        d.cameraFar        = 1000.0f;
        d.cameraFovAngleVertical = 1.0f;
        d.viewSpaceToMetersFactor = 1.0f;
        const ffxReturnCode_t rc = ffx.Dispatch(&ctx, &d.header);
        if (rc != FFX_API_RETURN_OK) { Log("  dispatch %d failed rc=%u", i, rc); return -1.0; }
        g.list->Close();
        ID3D12CommandList *lists[] = { g.list };
        g.queue->ExecuteCommandLists(1, lists);
        QueryPerformanceCounter(&t0);
        if (!WaitGpu(g)) { Log("  fence wait failed after dispatch %d (device lost?)", i); return -1.0; }
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        if (i >= 10) ms.push_back(1000.0 * double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart));
    }
    double sum = 0, mx = 0, mn = 1e9;
    for (double v : ms) { sum += v; if (v > mx) mx = v; if (v < mn) mn = v; }
    if (!ms.empty())
        Log("  %d frames: avg %.2f ms, min %.2f ms, max %.2f ms (frames 10..%d)",
            frames, sum / ms.size(), mn, mx, frames - 1);
    return ms.empty() ? -1.0 : sum / ms.size();
}

static bool DumpOutput(Gpu &g, ID3D12Resource *output, const char *path)
{
    // output: UAV -> COPY_SOURCE -> readback buffer -> BMP (RGBA16F assumed)
    if (FAILED(g.alloc->Reset()) || FAILED(g.list->Reset(g.alloc, nullptr))) return false;
    D3D12_RESOURCE_BARRIER bar = {};
    bar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource   = output;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g.list->ResourceBarrier(1, &bar);

    const UINT bpp = 8, row = kW * bpp;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = static_cast<UINT64>(row) * kH;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb = nullptr;
    if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&rb))) || !rb)
        return false;
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = output; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb;     dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dst.PlacedFootprint.Footprint.Width    = kW;
    dst.PlacedFootprint.Footprint.Height   = kH;
    dst.PlacedFootprint.Footprint.Depth    = 1;
    dst.PlacedFootprint.Footprint.RowPitch = row;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    g.list->Close();
    ID3D12CommandList *lists[] = { g.list };
    g.queue->ExecuteCommandLists(1, lists);
    const bool waited = WaitGpu(g);

    bool ok = false;
    const uint16_t *h16 = nullptr;
    if (waited && SUCCEEDED(rb->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint16_t **>(&h16)))) && h16)
    {
        // BMP rows are bottom-up and BGR; convert half->float->byte.
        std::vector<uint8_t> bgr(static_cast<size_t>(kW) * kH * 3);
        auto half2f = [](uint16_t h) -> float {
            uint32_t sign = (h & 0x8000u) << 16, exp = ((h >> 10) & 0x1Fu), man = h & 0x3FFu;
            uint32_t x;
            if (exp == 0 && man == 0)      x = sign;
            else if (exp == 31)            x = sign | 0x7F800000u | (man << 13);
            else { const uint32_t e = exp + 127 - 15; x = sign | (e << 23) | (man << 13); }
            float f; memcpy(&f, &x, sizeof f);
            return f;
        };
        for (UINT y = 0; y < kH; ++y)
        {
            const uint16_t *src_row = h16 + static_cast<size_t>(y) * kW * 4;
            uint8_t *dst_row = &bgr[static_cast<size_t>(kH - 1 - y) * kW * 3];
            for (UINT x = 0; x < kW; ++x)
            {
                dst_row[x * 3 + 0] = static_cast<uint8_t>(255.0f * fminf(1.0f, fmaxf(0.0f, half2f(src_row[x * 4 + 2]))));
                dst_row[x * 3 + 1] = static_cast<uint8_t>(255.0f * fminf(1.0f, fmaxf(0.0f, half2f(src_row[x * 4 + 1]))));
                dst_row[x * 3 + 2] = static_cast<uint8_t>(255.0f * fminf(1.0f, fmaxf(0.0f, half2f(src_row[x * 4 + 0]))));
            }
        }
        ok = WriteBMP24(path, bgr.data(), kW, kH);
        const D3D12_RANGE none = { 0, 0 };
        rb->Unmap(0, &none);
    }
    rb->Release();
    return ok;
}

int main(int argc, char **argv)
{
    bool ab = false;
    for (int i = 1; i < argc; ++i) if (strcmp(argv[i], "--ab") == 0) ab = true;

    char exe_dir[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe_dir, MAX_PATH);
    if (char *s = strrchr(exe_dir, '\\')) *s = 0;
    char logp[MAX_PATH];
    sprintf_s(logp, "%s\\ffx-smoke.log", exe_dir);
    if (fopen_s(&g_log, logp, "w") != 0) g_log = nullptr;

    Log("ffx-smoke: FSR Upscaling smoke test (FFX_UPSCALER_VERSION 0x%08X = %d.%d.%d)",
        FFX_UPSCALER_VERSION, FFX_UPSCALER_VERSION_MAJOR, FFX_UPSCALER_VERSION_MINOR, FFX_UPSCALER_VERSION_PATCH);

    // --- adapter: prefer AMD, print what we got
    Gpu g;
    IDXGIFactory4 *fact = nullptr;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void **>(&fact))))
    { Log("CreateDXGIFactory2 failed"); return 2; }
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
    IDXGIAdapter1 *chosen = amd ? amd : (any ? any : nullptr);
    if (!chosen) { Log("no hardware adapter"); return 2; }
    DXGI_ADAPTER_DESC1 dd = {};
    chosen->GetDesc1(&dd);
    char name[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, dd.Description, -1, name, sizeof name, nullptr, nullptr);
    Log("adapter: %s (vendor 0x%04X, device 0x%04X, VRAM %llu MB)", name, dd.VendorId, dd.DeviceId,
        static_cast<unsigned long long>(dd.DedicatedVideoMemory / (1024 * 1024)));

    if (FAILED(D3D12CreateDevice(chosen, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                                 reinterpret_cast<void **>(&g.dev))))
    { Log("D3D12CreateDevice failed"); return 2; }
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (FAILED(g.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&g.queue))) ||
        FAILED(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                             reinterpret_cast<void **>(&g.alloc))) ||
        FAILED(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr,
                                        __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g.list))) ||
        FAILED(g.list->Close()) ||
        FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g.fence))))
    { Log("D3D12 boilerplate failed"); return 2; }
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (amd) amd->Release();
    if (any) any->Release();
    fact->Release();

    // --- loader
    char loader_path[MAX_PATH];
    sprintf_s(loader_path, "%s\\amd_fidelityfx_loader_dx12.dll", exe_dir);
    HMODULE loader = LoadLibraryA(loader_path);
    if (loader == nullptr) { Log("cannot load %s (error %lu)", loader_path, GetLastError()); return 2; }
    ffxFunctions ffx = {};
    ffxLoadFunctions(&ffx, loader);
    if (!ffx.CreateContext || !ffx.Query || !ffx.Dispatch) { Log("loader exports missing"); return 2; }
    Log("loader: %s", loader_path);

    // --- versions offered on THIS device
    VersionInfo v = QueryVersions(ffx, g.dev);
    if (v.names.empty()) { Log("no upscaler versions offered -- loader cannot see the upscaler DLL?"); return 2; }
    uint64_t fsr3_id = 0;
    bool has41 = false;
    for (size_t i = 0; i < v.names.size(); ++i)
    {
        Log("  version[%u]: id 0x%016llX  \"%s\"", static_cast<unsigned>(i),
            static_cast<unsigned long long>(v.ids[i]), v.names[i].c_str());
        if (v.names[i].find("4.1") != std::string::npos) has41 = true;
        if (v.names[i].find("FSR3") != std::string::npos) fsr3_id = v.ids[i];
    }
    if (!has41)
        Log("NOTE: no 4.1 version offered -- the context below will fall back; check driver >= 26.10.1");

    // --- textures: gradient + circles + thin lines, depth ramp, zero motion
    ID3D12Resource *color = MakeTex(g, kW, kH, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *depth = MakeTex(g, kW, kH, DXGI_FORMAT_R32_FLOAT,         false, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *mv    = MakeTex(g, kW, kH, DXGI_FORMAT_R16G16_FLOAT,      false, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *output= MakeTex(g, kW, kH, DXGI_FORMAT_R16G16B16A16_FLOAT, true,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!color || !depth || !mv || !output) { Log("texture creation failed"); return 2; }

    UploadTex(g, color, kW, kH, 8, [](uint8_t *row, UINT y) {
        auto *px = reinterpret_cast<uint16_t *>(row);
        for (UINT x = 0; x < kW; ++x)
        {
            float r = float(x) / kW, gg = float(y) / kH, b = 1.0f - r;
            const float dc = hypotf(float(x) - kW * 0.3f, float(y) - kH * 0.5f);
            const float de = hypotf(float(x) - kW * 0.7f, float(y) - kH * 0.5f);
            if (dc < kH * 0.25f) { r = 1; gg = 1; b = 1; }
            if (fabsf(de - kH * 0.22f) < 1.5f) { r = 0; gg = 0; b = 0; }   // thin ring, hard for AA
            if (y % 7 == 0 && x > kW * 0.5f) { r = 1; gg = 0.5f; b = 0; }  // 1px lines
            px[x * 4 + 0] = F32ToF16(r);
            px[x * 4 + 1] = F32ToF16(gg);
            px[x * 4 + 2] = F32ToF16(b);
            px[x * 4 + 3] = F32ToF16(1.0f);
        }
    });
    UploadTex(g, depth, kW, kH, 4, [](uint8_t *row, UINT y) {
        auto *px = reinterpret_cast<float *>(row);
        for (UINT x = 0; x < kW; ++x) px[x] = 0.2f + 0.6f * (float(x) / kW * 0.5f + float(y) / kH * 0.5f);
    });
    UploadTex(g, mv, kW, kH, 4, [](uint8_t *row, UINT) {
        memset(row, 0, static_cast<size_t>(kW) * 4);   // static camera
    });

    // upload leaves them in COPY_DEST; move to the states the dispatch will claim
    {
        g.alloc->Reset(); g.list->Reset(g.alloc, nullptr);
        D3D12_RESOURCE_BARRIER bars[3] = {};
        ID3D12Resource *res[3] = { color, depth, mv };
        for (int i = 0; i < 3; ++i)
        {
            bars[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bars[i].Transition.pResource   = res[i];
            bars[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            bars[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            bars[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        g.list->ResourceBarrier(3, bars);
        g.list->Close();
        ID3D12CommandList *lists[] = { g.list };
        g.queue->ExecuteCommandLists(1, lists);
        WaitGpu(g);
    }

    // --- the test itself
    ffxReturnCode_t rc = FFX_API_RETURN_ERROR;
    Log("creating Upscale context (maxRender %ux%u, maxUpscale %ux%u, AUTO_EXPOSURE)...", kW, kH, kW, kH);
    ffxContext ctx = CreateUpscale(ffx, g.dev, 0, &rc);
    if (ctx == nullptr || rc != FFX_API_RETURN_OK) { Log("CreateContext failed rc=%u", rc); return 3; }
    const std::string provider = ProviderName(ffx, ctx);
    Log("context created; provider version: \"%s\"", provider.c_str());

    Log("dispatching 60 frames at 1920x1080 NativeAA 1:1...");
    const double avg = RunFrames(ffx, g, ctx, color, depth, mv, output, 60);
    if (avg < 0) { Log("dispatch loop FAILED"); ffx.DestroyContext(&ctx, nullptr); return 3; }

    char bmpp[MAX_PATH];
    sprintf_s(bmpp, "%s\\fsr41_smoke_out.bmp", exe_dir);
    if (DumpOutput(g, output, bmpp)) Log("output dumped: %s", bmpp);
    else Log("output dump failed (dispatch itself may still be fine)");

    // --- optional A/B against the FSR3 fallback
    if (ab && fsr3_id)
    {
        ffx.DestroyContext(&ctx, nullptr);
        Log("A/B: re-creating pinned to the FSR3 version (id 0x%016llX)...",
            static_cast<unsigned long long>(fsr3_id));
        ffxContext ctx3 = CreateUpscale(ffx, g.dev, fsr3_id, &rc);
        if (ctx3 && rc == FFX_API_RETURN_OK)
        {
            Log("FSR3 context provider: \"%s\"", ProviderName(ffx, ctx3).c_str());
            RunFrames(ffx, g, ctx3, color, depth, mv, output, 40);
            ffx.DestroyContext(&ctx3, nullptr);
        }
        else Log("FSR3 override context failed rc=%u", rc);
    }
    else if (ab) Log("A/B skipped: no FSR3 version in the list");

    if (!has41 || provider.find("4.1") == std::string::npos)
    {
        Log("RESULT: FAIL -- the provider is not FSR 4.1 on this GPU (\"%s\")", provider.c_str());
        ffx.DestroyContext(&ctx, nullptr);
        return 4;
    }
    ffx.DestroyContext(&ctx, nullptr);
    Log("RESULT: PASS -- FSR 4.1 created and dispatched standalone on this GPU");
    return 0;
}
