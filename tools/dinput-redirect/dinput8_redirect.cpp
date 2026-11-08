// dinput8.dll proxy + legacy-DirectX load diversion -- why this exists:
//
// SpellForce.exe (Phenomic) builds the ABSOLUTE path to d3d9.dll (and its
// DirectDraw/D3D8 kin) via GetSystemDirectory + LoadLibraryA, stepping past
// any local wrapper. Our IAT patch catches the exe's own imports, but the
// OPTIONS screen re-resolves d3d9 through other loader calls (GetProcAddress'd
// LoadLibraryExW among them), swaps dgVoodoo for the system DLL mid-session
// and crashes on the mixed state. So the diversion runs at two levels:
//
//   1. IAT patch on the exe for LoadLibraryA/CreateFileA (startup path);
//   2. global MinHook inline hooks on LoadLibraryA/W/ExA/ExW, armed on the
//      first DirectInput8Create call (main thread, no loader lock) -- nothing
//      in the process can load a system d3d8/d3d9/ddraw/d3dimm after that.
//
// Every diversion points at the dgVoodoo copies in this DLL's own folder, so
// all module uses and file reads see the same bytes. Nothing here touches the
// system: delete this file and the game is stock.

#include <windows.h>
#include <cstring>
#include <cstdint>
#include <cstdio>

#include "MinHook.h"
#include "hde32.h"

#pragma comment(lib, "kernel32.lib")

// ---------------------------------------------------------------- trace

static void Trace(const char *what, const char *from, const char *to)
{
    char self[MAX_PATH] = {}, line[2 * MAX_PATH + 64];
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&Trace), &m);
    if (m != nullptr && GetModuleFileNameA(m, self, MAX_PATH) != 0 && strrchr(self, '\\'))
        strcpy_s(strrchr(self, '\\') + 1, MAX_PATH - strlen(self), "fsr41-redirect.log");
    else
        strcpy_s(self, "C:\\fsr41-redirect.log");
    const size_t wlen = strlen(what);
    if (wlen > 400)
    {
        // oversized payloads (the module map) must bypass sprintf_s:
        // routing them through it once tripped __fastfail(RANGE_CHECK)
        // and killed the game before its own code ever ran
        FILE *big = nullptr;
        if (fopen_s(&big, self, "a") == 0 && big) { fputs(what, big); fputc(10, big); fclose(big); }
        return;
    }
    sprintf_s(line, "%s: %s%s%s\n", what, from, to ? " -> " : "", to ? to : "");
    FILE *f = nullptr;
    if (fopen_s(&f, self, "a") == 0 && f) { fputs(line, f); fclose(f); }
}

// ---------------------------------------------------------------- diversion core

static bool DirOf(HMODULE self, char *out, size_t cb)
{
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(self, path, MAX_PATH) == 0) return false;
    if (char *s = strrchr(path, '\\')) { *s = 0; strcpy_s(out, cb, path); return true; }
    return false;
}

static const char *const kLegacy[] = { "d3d8.dll", "d3d9.dll", "ddraw.dll", "d3dimm.dll" };
// d3d9 loads divert to the RENAMED copy (v3d9.dll): a loaded module named
// d3d9.dll makes ReShade install its D3D9 hooks, the feeder then binds to the
// D3D9 runtime and stops ("only Direct3D 10/11/... supported"). With the
// rename only the D3D11 runtime exists -- exactly what the feeder needs.
static const char *RedirectFileFor(const char *dll)
{
    return dll;   // no rename: the dinput8 proxy does the diverting
}

// If `file` names a legacy DirectX DLL, build the same-named path in this
// folder. Returns false (and leaves `out` untouched) for everything else.
static bool RedirectLegacyName(const char *file, char *out, size_t cb)
{
    if (file == nullptr) return false;
    const size_t n = strlen(file);
    for (const char *dll : kLegacy)
    {
        const size_t ln = strlen(dll);
        if (n < ln || _stricmp(file + n - ln, dll) != 0) continue;
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&RedirectLegacyName), &self);
        char dir[MAX_PATH] = {};
        if (self == nullptr || !DirOf(self, dir, sizeof dir)) return false;
        sprintf_s(out, cb, "%s\\%s", dir, RedirectFileFor(dll));
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- level 2: inline hooks

// The d3d9 entry-point patch is OFF unless FSR41_PATCH_D3D9=1 is set in the
// environment: it is a 1.61 experiment, and that engine checksums the loaded
// d3d9 module -- patched bytes make it abort before any video init.
static bool PatchD3D9ExportEnabled()
{
    char v[8] = {};
    return GetEnvironmentVariableA("FSR41_PATCH_D3D9", v, sizeof v) > 0 && v[0] == '1';
}

static void EnsureD3D9Patched();  // defined below -- called from the diversion paths

static HMODULE (WINAPI *RealLoadLibraryA)(LPCSTR)  = nullptr;
static HMODULE (WINAPI *RealLoadLibraryW)(LPCWSTR) = nullptr;
static HMODULE (WINAPI *RealLoadLibraryExA)(LPCSTR, HANDLE, DWORD)  = nullptr;
static HMODULE (WINAPI *RealLoadLibraryExW)(LPCWSTR, HANDLE, DWORD) = nullptr;

static HMODULE WINAPI MyLoadLibraryA(LPCSTR file)
{
    char redirect[MAX_PATH] = {};
    if (RedirectLegacyName(file, redirect, sizeof redirect) && _stricmp(file, redirect) != 0)
    {
        Trace("LoadLibraryA", file, redirect);
        const bool d3d9 = strlen(redirect) >= 8 && (_stricmp(redirect + strlen(redirect) - 8, "d3d9.dll") == 0 || _stricmp(redirect + strlen(redirect) - 8, "v3d9.dll") == 0);
        file = redirect;
        HMODULE h = RealLoadLibraryA(file);
        if (d3d9 && h != nullptr && PatchD3D9ExportEnabled()) EnsureD3D9Patched();
        return h;
    }
    return RealLoadLibraryA(file);
}

static HMODULE WINAPI MyLoadLibraryW(LPCWSTR file)
{
    char narrow[MAX_PATH] = {}, redirect[MAX_PATH] = {};
    if (file != nullptr && WideCharToMultiByte(CP_UTF8, 0, file, -1, narrow, sizeof narrow, nullptr, nullptr) > 0 &&
        RedirectLegacyName(narrow, redirect, sizeof redirect) && _stricmp(narrow, redirect) != 0)
    {
        wchar_t wide[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, redirect, -1, wide, MAX_PATH);
        Trace("LoadLibraryW", narrow, redirect);
        file = wide;
    }
    return RealLoadLibraryW(file);
}

static HMODULE WINAPI MyLoadLibraryExA(LPCSTR file, HANDLE h, DWORD flags)
{
    char redirect[MAX_PATH] = {};
    if (RedirectLegacyName(file, redirect, sizeof redirect) && _stricmp(file, redirect) != 0)
    {
        Trace("LoadLibraryExA", file, redirect);
        file = redirect;
    }
    return RealLoadLibraryExA(file, h, flags);
}

static HMODULE WINAPI MyLoadLibraryExW(LPCWSTR file, HANDLE h, DWORD flags)
{
    char narrow[MAX_PATH] = {}, redirect[MAX_PATH] = {};
    if (file != nullptr && WideCharToMultiByte(CP_UTF8, 0, file, -1, narrow, sizeof narrow, nullptr, nullptr) > 0 &&
        RedirectLegacyName(narrow, redirect, sizeof redirect) && _stricmp(narrow, redirect) != 0)
    {
        wchar_t wide[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, redirect, -1, wide, MAX_PATH);
        Trace("LoadLibraryExW", narrow, redirect);
        file = wide;
    }
    return RealLoadLibraryExW(file, h, flags);
}

static void InstallExitHook();   // defined below -- MinHook installs, safe off loader lock

static void InstallInlineHooks()
{
    static bool tried = false;
    if (tried) return;
    tried = true;
    if (MH_Initialize() != MH_OK)
    { Trace("hooks: MH_Initialize failed", "", nullptr); return; }
    struct { const char *name; void *detour; void **real; } hooks[] = {
        { "LoadLibraryA",   &MyLoadLibraryA,   reinterpret_cast<void **>(&RealLoadLibraryA) },
        { "LoadLibraryW",   &MyLoadLibraryW,   reinterpret_cast<void **>(&RealLoadLibraryW) },
        { "LoadLibraryExA", &MyLoadLibraryExA, reinterpret_cast<void **>(&RealLoadLibraryExA) },
        { "LoadLibraryExW", &MyLoadLibraryExW, reinterpret_cast<void **>(&RealLoadLibraryExW) },
    };
    for (auto &h : hooks)
    {
        void *target = reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), h.name));
        if (target == nullptr ||
            MH_CreateHook(target, h.detour, h.real) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
        {
            char msg[128];
            sprintf_s(msg, "hooks: cannot hook %s", h.name);
            Trace(msg, "", nullptr);
            return;
        }
    }
    Trace("hooks: LoadLibraryA/W/ExA/ExW armed", "", nullptr);
    InstallExitHook();   // MinHook needs no loader lock here (D3D9 hooks are armed from the diversion paths)
    // The Direct3DCreate9 entry patch + sanitized-mode wrap is armed from the
    // diversion paths themselves (see EnsureD3D9Patched), not from here: the
    // engine resolves the proc pointer before DirectInput8Create runs.
}

// ---------------------------------------------------------------- level 1: exe IAT (startup path)

static HMODULE (WINAPI *IatLoadLibraryA)(LPCSTR) = nullptr;

static HMODULE WINAPI IatMyLoadLibraryA(LPCSTR file)
{
    char redirect[MAX_PATH] = {};
    if (RedirectLegacyName(file, redirect, sizeof redirect) && _stricmp(file, redirect) != 0)
    {
        Trace("LoadLibraryA(iat)", file, redirect);
        const bool d3d9 = strlen(redirect) >= 8 && (_stricmp(redirect + strlen(redirect) - 8, "d3d9.dll") == 0 || _stricmp(redirect + strlen(redirect) - 8, "v3d9.dll") == 0);
        file = redirect;
        HMODULE h = IatLoadLibraryA(file);
        if (d3d9 && h != nullptr && PatchD3D9ExportEnabled()) EnsureD3D9Patched();
        return h;
    }
    return IatLoadLibraryA(file);
}

// The engine resolves Direct3DCreate9/9Ex through GetProcAddress at its
// first d3d9 load -- hand it OUR entry instead. No foreign bytes are touched
// (unlike the entry-point patch experiment), so nothing can be checksummed.
typedef void *IDirect3D9Like;
typedef IDirect3D9Like (WINAPI *PDirect3DCreate9)(UINT);
typedef long (WINAPI *PDirect3DCreate9Ex)(UINT, void **);
static PDirect3DCreate9   RealDirect3DCreate9 = nullptr;
static PDirect3DCreate9Ex RealDirect3DCreate9Ex = nullptr;

static FARPROC (WINAPI *IatGetProcAddress)(HMODULE, LPCSTR) = nullptr;
static int g_gpa_d3d_probes = 0;

static bool IsOurD3D9(HMODULE h)
{
    if (h == nullptr) return false;
    char path[MAX_PATH] = {}, dir[MAX_PATH] = {}, want[MAX_PATH] = {};
    if (GetModuleFileNameA(h, path, MAX_PATH) == 0) return false;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&IsOurD3D9), &self);
    if (self == nullptr || !DirOf(self, dir, sizeof dir)) return false;
    sprintf_s(want, "%s\\d3d9.dll", dir);
    static bool said = false;
    if (!said)
    {
        said = true;
        char dbg[2 * MAX_PATH];
        sprintf_s(dbg, "IsOurD3D9: path=[%s] want=[%s] eq=%d", path, want, _stricmp(path, want) == 0);
        Trace(dbg, "", nullptr);
    }
    return _stricmp(path, want) == 0;
}

static IDirect3D9Like WINAPI MyDirect3DCreate9(UINT sdk);   // defined in the wrap section
static long WINAPI MyDirect3DCreate9Ex(UINT sdk, void **out);

static FARPROC WINAPI IatMyGetProcAddress(HMODULE h, LPCSTR name)
{
    if (name != nullptr && strncmp(name, "Direct3D", 8) == 0 && g_gpa_d3d_probes < 8)
    {
        char probe[MAX_PATH + 64] = "GPA: ";
        if (h != nullptr) GetModuleFileNameA(h, probe + 5, MAX_PATH); else strcat_s(probe, "(null)");
        strcat_s(probe, " !");
        strcat_s(probe, name);
        Trace(probe, "", nullptr);
        ++g_gpa_d3d_probes;
    }
    if (name != nullptr && IsOurD3D9(h))
    {
        if (strcmp(name, "Direct3DCreate9") == 0)
        {
            if (RealDirect3DCreate9 == nullptr)
                RealDirect3DCreate9 = reinterpret_cast<PDirect3DCreate9>(IatGetProcAddress(h, name));
            if (RealDirect3DCreate9 != nullptr)
            {
                Trace("GetProcAddress: Direct3DCreate9 -> wrapped", "", nullptr);
                return reinterpret_cast<FARPROC>(&MyDirect3DCreate9);
            }
        }
        if (strcmp(name, "Direct3DCreate9Ex") == 0)
        {
            if (RealDirect3DCreate9Ex == nullptr)
                RealDirect3DCreate9Ex = reinterpret_cast<PDirect3DCreate9Ex>(IatGetProcAddress(h, name));
            if (RealDirect3DCreate9Ex != nullptr)
            {
                Trace("GetProcAddress: Direct3DCreate9Ex -> wrapped", "", nullptr);
                return reinterpret_cast<FARPROC>(&MyDirect3DCreate9Ex);
            }
        }
    }
    return IatGetProcAddress(h, name);
}

static void PatchExeIat()
{
    uint8_t *base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
    if (base == nullptr) return;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (rva == 0) return;
    auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + rva);
    for (; imp->Name != 0; ++imp)
    {
        const char *dll = reinterpret_cast<const char *>(base + imp->Name);
        if (_stricmp(dll, "kernel32.dll") != 0) continue;
        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
        auto names = imp->OriginalFirstThunk != 0
                       ? reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->OriginalFirstThunk) : thunk;
        for (int i = 0; thunk[i].u1.Function != 0; ++i)
        {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const char *fn = reinterpret_cast<const char *>(base + names[i].u1.AddressOfData + 2);
            DWORD old = 0;
            if (strcmp(fn, "LoadLibraryA") == 0)
            {
                if (VirtualProtect(&thunk[i].u1.Function, sizeof(void *), PAGE_READWRITE, &old))
                {
                    IatLoadLibraryA = reinterpret_cast<HMODULE (WINAPI *)(LPCSTR)>(thunk[i].u1.Function);
                    thunk[i].u1.Function = reinterpret_cast<uintptr_t>(&IatMyLoadLibraryA);
                    VirtualProtect(&thunk[i].u1.Function, sizeof(void *), old, &old);
                }
            }
            else if (strcmp(fn, "GetProcAddress") == 0)
            {
                if (VirtualProtect(&thunk[i].u1.Function, sizeof(void *), PAGE_READWRITE, &old))
                {
                    IatGetProcAddress = reinterpret_cast<FARPROC (WINAPI *)(HMODULE, LPCSTR)>(thunk[i].u1.Function);
                    thunk[i].u1.Function = reinterpret_cast<uintptr_t>(&IatMyGetProcAddress);
                    VirtualProtect(&thunk[i].u1.Function, sizeof(void *), old, &old);
                    Trace("hooks: exe GetProcAddress IAT hooked", "", nullptr);
                }
            }
            // keep scanning: kernel32 imports both functions, and a stray early
            // return here once cost us every hook (v6 bug -- it inspected only
            // the first thunk and installed nothing).
        }
    }
}

// ---------------------------------------------------------------- IDirect3D9 identity clamp
//
// The OPTIONS screen smashes its stack (0xC0000409, engine-side) on what
// dgVoodoo reports: either the 44-char adapter description ("... (dgVoodoo DX
// API Layer)") overflowing a 2003-era buffer, or the mode list of a 4K desktop
// overflowing a fixed UI array. Wrap the IDirect3D9 the engine gets from
// Direct3DCreate9 and clamp both: description/driver truncated to 28 chars,
// mode count capped. IDirect3D9 vtable slots (after IUnknown 0..2):
//   3 RegisterSoftwareDevice, 4 GetAdapterCount, 5 GetAdapterIdentifier,
//   6 GetAdapterModeCount, 7 EnumAdapterModes, 8 GetAdapterDisplayMode, ...

// The wrapped object's vtable points at OUR wrappers; the wrappers must call
// the ORIGINAL dgVoodoo methods, saved before the swap.
static void *g_orig_vt_slot[16] = {};

static void *vt_orig(int slot)
{
    return g_orig_vt_slot[slot & 15];
}

static void *vt_orig6() { return vt_orig(6); }
static void *vt_orig7() { return vt_orig(7); }
static void *vt_orig8() { return vt_orig(8); }

static const UINT  kMaxModesReported = 24;
static const DWORD kVtableEntries   = 64;   // IDirect3D9Ex is longer; be safe

typedef long D3D9HRESULT;                   // IDirect3D9Like: declared above

struct D3DAdapterIdentifier9Like
{
    char Driver[512];
    char Description[512];
    char DeviceName[32];
    DWORD DriverVersionHi, DriverVersionLo;
    DWORD VendorId, DeviceId, SubSysId;
    DWORD Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
};

static D3D9HRESULT WINAPI WrapGetAdapterCount(IDirect3D9Like self)
{
    (void)self;
    return 1;   // the engine chokes on the second (portrait) monitor
}

static D3D9HRESULT WINAPI WrapGetAdapterIdentifier(IDirect3D9Like self, UINT adapter, DWORD flags,
                                                   D3DAdapterIdentifier9Like *id)
{
    using PFn = D3D9HRESULT (WINAPI *)(IDirect3D9Like, UINT, DWORD, D3DAdapterIdentifier9Like *);
    return reinterpret_cast<PFn>(vt_orig(5))(self, adapter, flags, id);
}

// The 2003 engine cannot digest what a modern desktop reports through
// dgVoodoo: 54 modes per format overflows its fixed arrays, and a second
// (portrait) monitor confuses its adapter walk -- its log then fills with
// garbage and it asserts. Feed it the classic diet it was built for.
struct ClassicMode { UINT w, h; };
static const ClassicMode kClassicModes[] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 },
    { 1280, 960 }, { 1280, 1024 }, { 1600, 1200 },
    { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },   // match the reported display mode
};
static const UINT kClassicModeCount = sizeof(kClassicModes) / sizeof(kClassicModes[0]);

static int g_mode_logs = 0;

static D3D9HRESULT WINAPI WrapGetAdapterModeCount(IDirect3D9Like self, UINT adapter, DWORD fmt)
{
    (void)self; (void)adapter; (void)fmt;
    if (g_mode_logs < 8)
    {
        Trace("modeCount -> classic list", "", nullptr);
        ++g_mode_logs;
    }
    return static_cast<D3D9HRESULT>(kClassicModeCount);
}

struct D3DDisplayModeLike { UINT Width, Height, RefreshRate; DWORD Format; };

static D3D9HRESULT WINAPI WrapEnumAdapterModes(IDirect3D9Like self, UINT adapter, DWORD fmt, UINT mode,
                                               D3DDisplayModeLike *dm)
{
    using PFn = D3D9HRESULT (WINAPI *)(IDirect3D9Like, UINT, DWORD, UINT, D3DDisplayModeLike *);
    (void)self; (void)adapter;
    if (mode >= kClassicModeCount) return 0x887608C2;   // D3DERR_INVALIDCALL
    if (dm != nullptr)
    {
        dm->Width = kClassicModes[mode].w;
        dm->Height = kClassicModes[mode].h;
        dm->RefreshRate = 60;
        dm->Format = fmt;
    }
    return 0;   // D3D_OK
}

static D3D9HRESULT WINAPI WrapGetAdapterDisplayMode(IDirect3D9Like self, UINT adapter, D3DDisplayModeLike *dm)
{
    using PFn = D3D9HRESULT (WINAPI *)(IDirect3D9Like, UINT, D3DDisplayModeLike *);
    const D3D9HRESULT hr = reinterpret_cast<PFn>(vt_orig8())(self, adapter, dm);
    if (SUCCEEDED(hr) && dm != nullptr)
    {
        char msg[128];
        sprintf_s(msg, "displayMode(adapter=%u): %ux%u@%lu fmt=%lu", adapter,
                  dm->Width, dm->Height, dm->RefreshRate, dm->Format);
        Trace(msg, "", nullptr);
    }
    return hr;
}

static void *g_d3d9_vtable[kVtableEntries];   // shared: wrappers are stateless
static bool   g_d3d9_vtable_ready = false;

static IDirect3D9Like WrapD3D9Object(IDirect3D9Like obj)
{
    if (obj == nullptr) return nullptr;
    if (!g_d3d9_vtable_ready)
    {
        memcpy(g_d3d9_vtable, *reinterpret_cast<void ***>(obj), sizeof(g_d3d9_vtable));
        for (int s = 0; s < 16; ++s) g_orig_vt_slot[s] = g_d3d9_vtable[s];
        g_d3d9_vtable[4] = &WrapGetAdapterCount;
        g_d3d9_vtable[5] = &WrapGetAdapterIdentifier;
        g_d3d9_vtable[6] = &WrapGetAdapterModeCount;
        g_d3d9_vtable[7] = &WrapEnumAdapterModes;
        g_d3d9_vtable[8] = &WrapGetAdapterDisplayMode;
        g_d3d9_vtable_ready = true;
    }
    *reinterpret_cast<void ***>(obj) = g_d3d9_vtable;
    return obj;
}

static IDirect3D9Like WINAPI MyDirect3DCreate9(UINT sdk)
{
    const IDirect3D9Like obj = RealDirect3DCreate9(sdk);
    Trace("Direct3DCreate9 wrapped", "", nullptr);
    return WrapD3D9Object(obj);
}

static long WINAPI MyDirect3DCreate9Ex(UINT sdk, void **out)
{
    const long hr = RealDirect3DCreate9Ex(sdk, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
    {
        Trace("Direct3DCreate9Ex wrapped", "", nullptr);
        WrapD3D9Object(reinterpret_cast<IDirect3D9Like>(*out));
    }
    return hr;
}

// Patch the Direct3DCreate9 ENTRY POINT of the diverted dgVoodoo D3D9.dll
// with a 5-byte JMP, right after it loads: the engine caches the proc pointer
// at its first load -- before DirectInput8Create and thus before the MinHook
// hooks arm -- so an export patch applied inside the diversion itself is the
// only interception guaranteed to be in place in time. Only applied when the
// prologue is the classic position-independent hot-patch pattern.
#pragma pack(push, 1)
struct Jump5 { uint8_t opcode; int32_t rel; };
#pragma pack(pop)

static uint8_t *g_d3d9_trampoline = nullptr;    // RWX via VirtualAlloc: a .data
                                                // buffer is not executable (DEP) --
                                                // that, not an engine checksum, is
                                                // what killed the first attempt
static uint8_t *g_d3d9_trampoline_ex = nullptr; // the Direct3DCreate9Ex twin
static bool    g_d3d9_patched = false;

static void PatchExportByName(HMODULE d3d9, const char *export_name)
{
    uint8_t *fn = reinterpret_cast<uint8_t *>(GetProcAddress(d3d9, export_name));
    if (fn == nullptr) return;
    const bool is_ex = strcmp(export_name, "Direct3DCreate9Ex") == 0;
    // Disassemble whole instructions until >= 5 bytes are displaced, so ANY
    // prologue works (dgVoodoo's export is a forwarder into dgvdumd.dll whose
    // prologue is not the classic hot-patch pattern).
    uint32_t displaced = 0;
    while (displaced < 5)
    {
        hde32s ins = {};
        const uint32_t len = hde32_disasm(fn + displaced, &ins);
        if (len == 0 || ins.flags & F_RELATIVE || ins.flags & F_ERROR)   // rel branch/call: cannot relocate blindly
        { Trace("d3d9 export: un-relocatable prologue, not patched", export_name, nullptr); return; }
        displaced += len;
        if (displaced > 12) { Trace("d3d9 export: prologue too long, not patched", export_name, nullptr); return; }
    }

    uint8_t *tramp = is_ex
        ? (g_d3d9_trampoline_ex != nullptr ? g_d3d9_trampoline_ex
            : (g_d3d9_trampoline_ex = static_cast<uint8_t *>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))))
        : (g_d3d9_trampoline != nullptr ? g_d3d9_trampoline
            : (g_d3d9_trampoline = static_cast<uint8_t *>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))));
    if (tramp == nullptr) return;
    memcpy(tramp, fn, displaced);
    auto *back = reinterpret_cast<Jump5 *>(tramp + displaced);
    back->opcode = 0xE9;
    back->rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(fn + displaced) -
                                     reinterpret_cast<intptr_t>(back + 1));

    // our wrapper calls the ORIGINAL through the freshly built trampoline
    if (is_ex)
    {
        if (RealDirect3DCreate9Ex == nullptr)
            RealDirect3DCreate9Ex = reinterpret_cast<PDirect3DCreate9Ex>(static_cast<void *>(tramp));
    }
    else
    {
        if (RealDirect3DCreate9 == nullptr)
            RealDirect3DCreate9 = reinterpret_cast<PDirect3DCreate9>(static_cast<void *>(tramp));
    }

    DWORD old = 0;
    if (!VirtualProtect(fn, displaced, PAGE_EXECUTE_READWRITE, &old)) return;
    void *detour = is_ex ? reinterpret_cast<void *>(&MyDirect3DCreate9Ex)
                         : reinterpret_cast<void *>(&MyDirect3DCreate9);
    auto *jmp = reinterpret_cast<Jump5 *>(fn);
    jmp->opcode = 0xE9;
    jmp->rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(detour) -
                                    reinterpret_cast<intptr_t>(fn + 5));
    VirtualProtect(fn, displaced, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, displaced);
    Trace("d3d9 export: entry patched", export_name, nullptr);
}

// Called from both diversion paths with the freshly loaded module -- before
// the engine can resolve anything from it.
static void EnsureD3D9Patched()
{
    if (g_d3d9_patched) return;
    char dir[MAX_PATH] = {}, path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&EnsureD3D9Patched), &self);
    if (self == nullptr || !DirOf(self, dir, sizeof dir)) return;
    sprintf_s(path, "%s\\d3d9.dll", dir);
    HMODULE d3d9 = GetModuleHandleA(path);
    if (d3d9 == nullptr) return;
    PatchExportByName(d3d9, "Direct3DCreate9");     // the classic path
    PatchExportByName(d3d9, "Direct3DCreate9Ex");   // the no-shim engine path
    g_d3d9_patched = true;
}

// ---------------------------------------------------------------- crash tracer
// Vectored handler: the engine's own crash dialog unwinds the faulting
// context before its CrashData.mdmp is written, so the interesting EIP is
// lost. Catch the ACCESS_VIOLATION first and log it here.
// module snapshot for attribution, taken at the first exception
#define WIN32_LEAN_AND_MEAN_SKIP
#include <tlhelp32.h>
static char g_mod_map[8192];   // "base size name" lines

static void SnapshotModules()
{
    if (g_mod_map[0] != 0) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me = { sizeof(me) };
    g_mod_map[0] = 0;
    if (Module32First(snap, &me))
    {
        do
        {
            char line[128];
            const char nl[2] = { 10, 0 };
            sprintf_s(line, "%p %06lX %s", me.modBaseAddr, me.modBaseSize, me.szModule);
            strcat_s(line, nl);
            if (strlen(g_mod_map) + strlen(line) < sizeof(g_mod_map) - 1)
                strcat_s(g_mod_map, line);
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
}

static LONG WINAPI CrashTracer(EXCEPTION_POINTERS *ep)
{
    static bool busy = false;
    static int seq = 0;
    if (busy) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    // C++ EH (0xE06D7363) and normal status messages are noise; the rest --
    // AV, stack overflow, fastfail, illegal instruction -- all get logged
    if (er->ExceptionCode == 0xE06D7363 || er->ExceptionCode == 0x4000001F) return EXCEPTION_CONTINUE_SEARCH;
    busy = true;
    SnapshotModules();
    Trace(g_mod_map, "", nullptr);
    const uint8_t *base = reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
    const CONTEXT *c = ep->ContextRecord;
    char msg[160];
    sprintf_s(msg, "AV #%d: eip=0x%08X addr=0x%p esp=0x%08X ebp=0x%08X eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X",
              ++seq, c->Eip, er->ExceptionInformation[1], c->Esp, c->Ebp,
              c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
    Trace(msg, "", nullptr);
    // module bases for offline attribution
    char mb[256] = "bases:";
    static const char *const kMods[] = { "SpellForce.exe", "d3d9.dll", "d3d11.dll", "dxgi.dll", "dinput8.dll", "KERNEL32.DLL" };
    for (const char *m : kMods)
    {
        const HMODULE h = GetModuleHandleA(m);
        char piece[64];
        sprintf_s(piece, " %s=0x%p", m, static_cast<void *>(h));
        strcat_s(mb, piece);
    }
    Trace(mb, "", nullptr);
    const uint32_t *stk = reinterpret_cast<const uint32_t *>(c->Esp);
    for (int i = 0; i < 64; ++i)
    {
        char line[96];
        sprintf_s(line, "  stk+%02x: 0x%08X", i * 4, stk[i]);
        Trace(line, "", nullptr);
    }
    busy = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- ExitProcess tracer
static void (WINAPI *RealExitProcess)(UINT) = nullptr;

static void WINAPI MyExitProcess(UINT code)
{
    void *frames[24] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    const uint8_t *base = reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
    char msg[96];
    sprintf_s(msg, "ExitProcess(%u), %u frames:", code, n);
    Trace(msg, "", nullptr);
    for (USHORT i = 0; i < n; ++i)
    {
        const uintptr_t v = reinterpret_cast<uintptr_t>(frames[i]);
        if (reinterpret_cast<uintptr_t>(base) <= v && v < reinterpret_cast<uintptr_t>(base) + 0xA00000)
        {
            char line[96];
            sprintf_s(line, "  exit+%02u: engine+0x%X", i, static_cast<unsigned>(v - reinterpret_cast<uintptr_t>(base)));
            Trace(line, "", nullptr);
        }
    }
    RealExitProcess(code);
}

static void LogEngineFrames(const char *tag)
{
    void *frames[24] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    const uint8_t *base = reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
    char msg[96];
    sprintf_s(msg, "%s, %u frames:", tag, n);
    Trace(msg, "", nullptr);
    for (USHORT i = 0; i < n; ++i)
    {
        const uintptr_t v = reinterpret_cast<uintptr_t>(frames[i]);
        if (reinterpret_cast<uintptr_t>(base) <= v && v < reinterpret_cast<uintptr_t>(base) + 0xA00000)
        {
            char line[96];
            sprintf_s(line, "  +%02u: engine+0x%X", i, static_cast<unsigned>(v - reinterpret_cast<uintptr_t>(base)));
            Trace(line, "", nullptr);
        }
    }
}

static int (WINAPI *RealMessageBoxA)(HWND, LPCSTR, LPCSTR, UINT) = nullptr;
static int WINAPI MyMessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT t)
{
    char msg[512];
    sprintf_s(msg, "MessageBoxA: [%s] %s", caption ? caption : "", text ? text : "");
    Trace(msg, "", nullptr);
    LogEngineFrames("msgbox caller");
    return RealMessageBoxA(w, text, caption, t);
}

static void InstallExitHook()
{
    void *target = reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitProcess"));
    if (target == nullptr) return;
    if (MH_CreateHook(target, &MyExitProcess, reinterpret_cast<void **>(&RealExitProcess)) == MH_OK)
        MH_EnableHook(target);
    void *mb = reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "MessageBoxA"));
    if (mb != nullptr && MH_CreateHook(mb, &MyMessageBoxA, reinterpret_cast<void **>(&RealMessageBoxA)) == MH_OK)
        MH_EnableHook(mb);
}



// ---------------------------------------------------------------- dinput8 forward

static HMODULE g_real_dinput8 = nullptr;

typedef long HRESULT_;
struct Guid { uint32_t a; uint16_t b, c; uint8_t d[8]; };
typedef HRESULT_ (WINAPI *PDirectInput8Create)(HINSTANCE, DWORD, const Guid &, void **, IUnknown *);

static HMODULE RealDInput8()
{
    if (g_real_dinput8 != nullptr) return g_real_dinput8;
    wchar_t path[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n + 16 >= MAX_PATH) return nullptr;
    wcscat_s(path, L"\\dinput8.dll");
    g_real_dinput8 = LoadLibraryW(path);
    return g_real_dinput8;
}

extern "C" __declspec(dllexport) HRESULT_ WINAPI DirectInput8Create(
    HINSTANCE hinst, DWORD version, const Guid &riidltf, void **ppvOut, IUnknown *outer)
{
    InstallInlineHooks();   // main thread, past loader lock -- arm the global hooks
    HMODULE real = RealDInput8();
    if (real == nullptr) return 0x8007007E;
    auto p = reinterpret_cast<PDirectInput8Create>(GetProcAddress(real, "DirectInput8Create"));
    if (p == nullptr) return 0x8007FFFF;
    return p(hinst, version, riidltf, ppvOut, outer);
}

// ----------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        PatchExeIat();
        AddVectoredExceptionHandler(1, &CrashTracer);
    }
    return TRUE;
}
