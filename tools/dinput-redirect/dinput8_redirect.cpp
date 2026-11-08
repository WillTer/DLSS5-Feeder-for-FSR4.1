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
        sprintf_s(out, cb, "%s\\%s", dir, dll);
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
        const bool d3d9 = strlen(redirect) >= 8 && _stricmp(redirect + strlen(redirect) - 8, "d3d9.dll") == 0;
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
        const bool d3d9 = strlen(redirect) >= 8 && _stricmp(redirect + strlen(redirect) - 8, "d3d9.dll") == 0;
        file = redirect;
        HMODULE h = IatLoadLibraryA(file);
        if (d3d9 && h != nullptr && PatchD3D9ExportEnabled()) EnsureD3D9Patched();
        return h;
    }
    return IatLoadLibraryA(file);
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
            if (strcmp(fn, "LoadLibraryA") != 0) continue;
            DWORD old = 0;
            if (VirtualProtect(&thunk[i].u1.Function, sizeof(void *), PAGE_READWRITE, &old))
            {
                IatLoadLibraryA = reinterpret_cast<HMODULE (WINAPI *)(LPCSTR)>(thunk[i].u1.Function);
                thunk[i].u1.Function = reinterpret_cast<uintptr_t>(&IatMyLoadLibraryA);
                VirtualProtect(&thunk[i].u1.Function, sizeof(void *), old, &old);
            }
            return;
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

static const UINT  kMaxModesReported = 24;
static const DWORD kVtableEntries   = 64;   // IDirect3D9Ex is longer; be safe

typedef long D3D9HRESULT;
typedef void *IDirect3D9Like;               // we only touch the vtable pointer

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

static D3D9HRESULT WINAPI WrapGetAdapterIdentifier(IDirect3D9Like self, UINT adapter, DWORD flags,
                                                   D3DAdapterIdentifier9Like *id)
{
    using PFn = D3D9HRESULT (WINAPI *)(IDirect3D9Like, UINT, DWORD, D3DAdapterIdentifier9Like *);
    void **vt = *reinterpret_cast<void ***>(self);
    D3D9HRESULT hr = reinterpret_cast<PFn>(vt[5])(self, adapter, flags, id);
    if (SUCCEEDED(hr) && id != nullptr)
    {
        id->Description[28] = 0;   // "dgVoodoo DX API Layer" suffix gone
        id->Driver[28] = 0;
    }
    return hr;
}

static D3D9HRESULT WINAPI WrapGetAdapterModeCount(IDirect3D9Like self, UINT adapter, DWORD fmt)
{
    using PFn = D3D9HRESULT (WINAPI *)(IDirect3D9Like, UINT, DWORD);
    void **vt = *reinterpret_cast<void ***>(self);
    reinterpret_cast<PFn>(vt[6])(self, adapter, fmt);   // touch, then ignore
    return 1;   // one sanitized mode (see below)
}

struct D3DDisplayModeLike { UINT Width, Height, RefreshRate; DWORD Format; };

static D3D9HRESULT WINAPI WrapEnumAdapterModes(IDirect3D9Like self, UINT adapter, DWORD fmt, UINT mode,
                                               D3DDisplayModeLike *dm)
{
    using PFn = D3D9HRESULT (WINAPI *)(IDirect3D9Like, UINT, DWORD, UINT, D3DDisplayModeLike *);
    void **vt = *reinterpret_cast<void ***>(self);
    if (dm == nullptr)
        return reinterpret_cast<PFn>(vt[7])(self, adapter, fmt, mode, dm);
    if (mode > 0) return 0x887608C2;   // D3DERR_INVALIDCALL: we report one mode
    dm->Width = 3840; dm->Height = 2160; dm->RefreshRate = 60; dm->Format = fmt;
    return 0;   // D3D_OK
}

static D3D9HRESULT WINAPI WrapGetAdapterDisplayMode(IDirect3D9Like self, UINT adapter, D3DDisplayModeLike *dm)
{
    using PFn = D3D9HRESULT (WINAPI *)(IDirect3D9Like, UINT, D3DDisplayModeLike *);
    void **vt = *reinterpret_cast<void ***>(self);
    if (dm == nullptr)
        return reinterpret_cast<PFn>(vt[8])(self, adapter, dm);
    dm->Width = 3840; dm->Height = 2160; dm->RefreshRate = 60;
    return 0;
}

static void *g_d3d9_vtable[kVtableEntries];   // shared: wrappers are stateless
static bool   g_d3d9_vtable_ready = false;

static IDirect3D9Like WrapD3D9Object(IDirect3D9Like obj)
{
    if (obj == nullptr) return nullptr;
    if (!g_d3d9_vtable_ready)
    {
        memcpy(g_d3d9_vtable, *reinterpret_cast<void ***>(obj), sizeof(g_d3d9_vtable));
        g_d3d9_vtable[5] = &WrapGetAdapterIdentifier;
        g_d3d9_vtable[6] = &WrapGetAdapterModeCount;
        g_d3d9_vtable[7] = &WrapEnumAdapterModes;
        g_d3d9_vtable[8] = &WrapGetAdapterDisplayMode;
        g_d3d9_vtable_ready = true;
    }
    *reinterpret_cast<void ***>(obj) = g_d3d9_vtable;
    return obj;
}

typedef IDirect3D9Like (WINAPI *PDirect3DCreate9)(UINT);
static PDirect3DCreate9 RealDirect3DCreate9 = nullptr;

static IDirect3D9Like WINAPI MyDirect3DCreate9(UINT sdk)
{
    const IDirect3D9Like obj = RealDirect3DCreate9(sdk);
    Trace("Direct3DCreate9 wrapped", "", nullptr);
    return WrapD3D9Object(obj);
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

static uint8_t g_d3d9_trampoline[16];   // displaced bytes + JMP back
static bool    g_d3d9_patched = false;

static void PatchD3D9Export(HMODULE d3d9)
{
    if (g_d3d9_patched || d3d9 == nullptr) return;
    uint8_t *fn = reinterpret_cast<uint8_t *>(GetProcAddress(d3d9, "Direct3DCreate9"));
    if (fn == nullptr) return;
    static const uint8_t kClassic[] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };  // mov edi,edi; push ebp; mov ebp,esp
    if (memcmp(fn, kClassic, sizeof kClassic) != 0)
    { Trace("d3d9 export: unfamiliar prologue, not patched", "", nullptr); g_d3d9_patched = true; return; }

    memcpy(g_d3d9_trampoline, fn, 5);
    auto *back = reinterpret_cast<Jump5 *>(g_d3d9_trampoline + 5);
    back->opcode = 0xE9;
    back->rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(fn + 5) -
                                     reinterpret_cast<intptr_t>(back + 1));

    DWORD old = 0;
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    auto *jmp = reinterpret_cast<Jump5 *>(fn);
    jmp->opcode = 0xE9;
    jmp->rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(&MyDirect3DCreate9) -
                                    reinterpret_cast<intptr_t>(fn + 5));
    VirtualProtect(fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    RealDirect3DCreate9 = reinterpret_cast<PDirect3DCreate9>(static_cast<void *>(g_d3d9_trampoline));
    g_d3d9_patched = true;
    Trace("d3d9 export: Direct3DCreate9 entry patched", "", nullptr);
}

// Called from both diversion paths with the freshly loaded module -- before
// the engine can resolve anything from it.
static void EnsureD3D9Patched()
{
    char dir[MAX_PATH] = {}, path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&EnsureD3D9Patched), &self);
    if (self == nullptr || !DirOf(self, dir, sizeof dir)) return;
    sprintf_s(path, "%s\\d3d9.dll", dir);
    HMODULE d3d9 = GetModuleHandleA(path);
    if (d3d9 != nullptr) PatchD3D9Export(d3d9);
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
    if (reason == DLL_PROCESS_ATTACH) PatchExeIat();
    return TRUE;
}
