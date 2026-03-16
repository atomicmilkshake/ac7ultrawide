// ac7ultrawide.asi
// Runtime memory patcher for ACE COMBAT 7 ultrawide support.
// Loaded via Ultimate ASI Loader (version.dll in game root).
//
// Replaces magic.py's disk-based exe patching with in-memory patching:
//   - Removes letterbox black bars
//   - Sets horizontal FOV to match your ultrawide aspect ratio
//   - Updates Mods/hudtextfix.ini with the correct HUD shift value
//   - Hooks D3D11 CreatePixelShader to fix HUD positioning directly
//
// Requires: the ORIGINAL (unpatched) Ace7Game.exe.
// If magic.py was previously run, restore Ace7Game.exe from the .bak or
// .exe_TIMESTAMP backup before switching to this ASI.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <MinHook.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static std::ofstream g_log;

static void Log(const char* msg)
{
    if (g_log.is_open())
        g_log << msg << "\n";
}

static void Logf(const char* fmt, ...)
{
    if (!g_log.is_open()) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_log << buf << "\n";
}

// ---------------------------------------------------------------------------
// Memory utilities
// ---------------------------------------------------------------------------

// Exact byte-pattern scan over [base, base+size).
// Returns a pointer to the first match, or nullptr.
static uint8_t* FindPattern(uint8_t* base, size_t size,
                             const uint8_t* pat, size_t patLen)
{
    if (patLen == 0 || patLen > size) return nullptr;
    const size_t limit = size - patLen;
    for (size_t i = 0; i <= limit; ++i)
        if (memcmp(base + i, pat, patLen) == 0)
            return base + i;
    return nullptr;
}

// Make [addr, addr+size) writable, copy data, restore protection.
static bool PatchBytes(void* addr, const void* data, size_t size)
{
    DWORD old;
    if (!VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(addr, data, size);
    VirtualProtect(addr, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, size);
    return true;
}

// ---------------------------------------------------------------------------
// FOV context-scan (handles already-patched exe on disk)
//
// The unique marker for the FOV location is the 3 bytes *after* the 3-byte
// FOV value: 0x3C 0xD8 0xF5. On an unpatched exe the full sequence is
// 35 FA 0E | 3C D8 F5 (90 deg, 16:9). On an already-patched exe the first
// three bytes are different. We find the context suffix and validate that
// the preceding bytes look like a plausible FOV integer (0 .. 0x7FFFFF).
// ---------------------------------------------------------------------------
static uint8_t* FindFovLocation(uint8_t* base, size_t imageSize)
{
    // Try exact original pattern first (unpatched exe).
    static const uint8_t kOrigPat[] = { 0x35, 0xFA, 0x0E, 0x3C, 0xD8, 0xF5 };
    uint8_t* p = FindPattern(base, imageSize, kOrigPat, sizeof(kOrigPat));
    if (p) return p;

    // Fallback: scan for the context suffix 3C D8 F5 and check the 3 bytes
    // before it form a reasonable FOV integer.
    static const uint8_t kCtx[] = { 0x3C, 0xD8, 0xF5 };
    for (size_t i = 3; i + 3 <= imageSize; ++i)
    {
        if (memcmp(base + i, kCtx, 3) != 0) continue;
        uint32_t candidate = (uint32_t)base[i - 3]
                           | ((uint32_t)base[i - 2] << 8)
                           | ((uint32_t)base[i - 1] << 16);
        // Valid 3-byte FOV ints for aspect ratios between 4:3 and 32:9
        // map to roughly 0x050000..0x7FFFFF.
        if (candidate >= 0x050000 && candidate <= 0x7FFFFF)
            return base + (i - 3);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// HUD ini patching
//
// hudtextfix.ini contains a line:
//   add ${register}, ${register}, l(VALUE)
// We find that line and replace VALUE with the new shift.
// ---------------------------------------------------------------------------
static bool UpdateHudShift(const std::string& iniPath, double shift)
{
    std::ifstream in(iniPath, std::ios::binary);
    if (!in.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    in.close();

    // Marker that uniquely identifies the live (non-commented) replace line.
    // The file contains the literal text:  ${0}\nadd ${register}, ${register}, l(VALUE)
    // The commented copy above it starts with "; add" so it won't match here.
    const std::string marker = "\\nadd ${register}, ${register}, l(";
    const auto markerPos = content.find(marker);
    if (markerPos == std::string::npos) return false;

    const size_t valueStart = markerPos + marker.size();
    const size_t valueEnd   = content.find(')', valueStart);
    if (valueEnd == std::string::npos) return false;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4) << shift;
    content.replace(valueStart, valueEnd - valueStart, oss.str());

    std::ofstream out(iniPath, std::ios::binary);
    if (!out.is_open()) return false;
    out << content;
    return true;
}

// ============================================================================
// D3D11 shader hook — HUD position correction via MinHook + D3DCompile
//
// AC7's HUD pixel shaders multiply cb1[127].y (aspect ratio) by the literal
// float 16/9 (1.777778).  For ultrawide, this produces wrong HUD positions.
//
// We hook CreatePixelShader via MinHook, detect shaders containing the 16/9
// literal, and substitute a D3DCompile'd replacement with the correct HUD
// shift baked in.  D3DCompile produces valid DXBC with correct checksums,
// avoiding the custom-hash nightmare of in-place bytecode patching.
// ============================================================================

// HUD shift in clip-space.  Computed at startup from screen resolution.
// If 0.0, no patching is performed (16:9 or narrower monitor).
static float g_hudShift = 0.0f;

// Compiled replacement shader blob (cached from D3DCompile at startup).
static ID3DBlob* g_compiledHudPS = nullptr;

// D3DCompile function pointer (loaded from d3dcompiler_47.dll at startup).
typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
    ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
    ID3DBlob**, ID3DBlob**);
static PFN_D3DCompile g_pD3DCompile = nullptr;

// Shader hook function pointers.
typedef HRESULT (STDMETHODCALLTYPE *PFN_CPS)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
static PFN_CPS g_origCPS = nullptr;

// ---------------------------------------------------------------------------
// HLSL template for the primary HUD pixel shader (PS[436], len=1104).
// Based on 3Dmigoto's proven 9958a636cbef5557-ps_replace.txt.
// The %.6f placeholder is replaced with g_hudShift at compile time.
// ---------------------------------------------------------------------------
static const char kHudPS_HLSL_Fmt[] =
    "cbuffer cb1 : register(b1) { float4 cb1[129]; }\n"
    "cbuffer cb0 : register(b0) { float4 cb0[21]; }\n"
    "Texture2D<float4> t1 : register(t1);\n"
    "Texture2D<float4> t0 : register(t0);\n"
    "SamplerState s0_s : register(s0);\n"
    "#define cmp -\n"
    "void main(float2 v0 : TEXCOORD0, out float4 o0 : SV_Target0) {\n"
    "  float4 r0,r1,r2,r3;\n"
    "  r0.x = cmp(1 < cb1[127].y);\n"
    "  r0.y = 1.77777779 * cb1[127].y;\n"
    "  r0.y += %.6f;\n"
    "  r0.y = cb1[127].x / r0.y;\n"
    "  r0.zw = v0.xy * cb1[128].xy + -cb1[126].xy;\n"
    "  r1.yz = cb1[127].zw * r0.zw;\n"
    "  r0.y = r1.y * r0.y;\n"
    "  r1.x = r0.x ? r0.y : r1.y;\n"
    "  r0.xy = cmp(r1.xz >= cb0[20].xy);\n"
    "  r0.zw = cmp(cb0[20].zw >= r1.xz);\n"
    "  r0.x = r0.z ? r0.x : 0;\n"
    "  r0.x = r0.y ? r0.x : 0;\n"
    "  r0.yz = cmp(r1.xz >= cb0[19].xy);\n"
    "  r1.yw = cmp(cb0[19].zw >= r1.xz);\n"
    "  r2.xyzw = t1.Sample(s0_s, v0.xy).xyzw;\n"
    "  r0.y = r0.y ? r1.y : 0;\n"
    "  r0.xy = r0.wz ? r0.xy : 0;\n"
    "  r0.y = r1.w ? r0.y : 0;\n"
    "  r0.x = (int)r0.y | (int)r0.x;\n"
    "  r0.y = 1 + -r2.w;\n"
    "  r1.xyzw = t0.Sample(s0_s, v0.xy).xyzw;\n"
    "  r3.xyzw = r1.xyzw * r2.wwww;\n"
    "  r3.xyzw = r0.yyyy * r3.xyzw + r2.xyzw;\n"
    "  r0.y = 1 + -r1.w;\n"
    "  r1.xyzw = r0.yyyy * r2.xyzw + r1.xyzw;\n"
    "  o0.xyzw = r0.xxxx ? r3.xyzw : r1.xyzw;\n"
    "}\n";

// Check if DXBC bytecode contains the 16/9 float literal (DWORD-aligned scan).
static bool ContainsAspectFloat(const void* bytecode, SIZE_T len)
{
    const float kTarget = 16.0f / 9.0f;
    const auto* bytes = static_cast<const uint8_t*>(bytecode);
    for (SIZE_T i = 0; i + 4 <= len; i += 4) {
        float val;
        memcpy(&val, bytes + i, 4);
        if (fabsf(val - kTarget) < 0.0001f)
            return true;
    }
    return false;
}

// Compile HUD replacement PS from HLSL template with shift value baked in.
static bool CompileHudShader()
{
    if (!g_pD3DCompile || g_hudShift == 0.0f) return false;

    char hlsl[2048];
    snprintf(hlsl, sizeof(hlsl), kHudPS_HLSL_Fmt, g_hudShift);

    ID3DBlob* errors = nullptr;
    HRESULT hr = g_pD3DCompile(
        hlsl, strlen(hlsl),
        "HudPS", nullptr, nullptr,
        "main", "ps_5_0",
        0, 0,
        &g_compiledHudPS, &errors);

    if (FAILED(hr)) {
        if (errors) {
            Logf("D3DCompile HUD PS failed: %s",
                 (const char*)errors->GetBufferPointer());
            errors->Release();
        } else {
            Logf("D3DCompile HUD PS failed: hr=0x%08X", (unsigned)hr);
        }
        return false;
    }
    if (errors) errors->Release();

    Logf("D3DCompile HUD PS: OK (%zu bytes, shift=%.6f)",
         g_compiledHudPS->GetBufferSize(), g_hudShift);
    return true;
}

static HRESULT STDMETHODCALLTYPE HookCreatePixelShader(
    ID3D11Device*       device,
    const void*         pBytecode,
    SIZE_T              len,
    ID3D11ClassLinkage* pCL,
    ID3D11PixelShader** ppPS)
{
    // Detect shaders containing the 16/9 aspect literal and substitute.
    if (g_compiledHudPS && ContainsAspectFloat(pBytecode, len)) {
        // PS[436] (len=1104): primary HUD shader — use compiled replacement.
        if (len == 1104) {
            HRESULT hr = g_origCPS(device,
                                    g_compiledHudPS->GetBufferPointer(),
                                    g_compiledHudPS->GetBufferSize(),
                                    pCL, ppPS);
            if (SUCCEEDED(hr)) {
                Logf("PS HUD patch: OK (original len=%zu, replacement len=%zu)",
                     len, g_compiledHudPS->GetBufferSize());
                g_log.flush();
                return hr;
            }
            Logf("PS HUD patch: CreatePixelShader failed (hr=0x%08X)",
                 (unsigned)hr);
            g_log.flush();
        } else {
            Logf("PS HUD candidate: len=%zu contains 16/9 float (no replacement yet)",
                 len);
            g_log.flush();
        }
    }

    return g_origCPS(device, pBytecode, len, pCL, ppPS);
}

// D3D11CreateDevice hooks — intercept device creation to install PS hook.
typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static PFN_D3D11CreateDevice g_origD3D11CreateDevice = nullptr;

typedef HRESULT (WINAPI *PFN_D3D11CreateDASC)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*,
    IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static PFN_D3D11CreateDASC g_origD3D11CreateDASC = nullptr;

static bool g_psHooked = false;

static void HookPSCreation(ID3D11Device* dev)
{
    if (g_psHooked || !dev) return;
    g_psHooked = true;

    void** vtable = *reinterpret_cast<void***>(dev);
    MH_STATUS st = MH_CreateHook(vtable[15],
                                  reinterpret_cast<void*>(HookCreatePixelShader),
                                  reinterpret_cast<void**>(&g_origCPS));
    if (st == MH_OK) {
        MH_EnableHook(vtable[15]);
        Log("MinHook: CreatePixelShader hooked OK");
    } else {
        Logf("MinHook: CreatePixelShader hook failed (%d)", st);
    }
}

static HRESULT WINAPI HookD3D11CreateDevice(
    IDXGIAdapter* pA, D3D_DRIVER_TYPE dt, HMODULE sw, UINT fl,
    const D3D_FEATURE_LEVEL* pFL, UINT nFL, UINT sdk,
    ID3D11Device** ppDev, D3D_FEATURE_LEVEL* pFLOut, ID3D11DeviceContext** ppCtx)
{
    HRESULT hr = g_origD3D11CreateDevice(pA, dt, sw, fl, pFL, nFL, sdk,
                                          ppDev, pFLOut, ppCtx);
    if (SUCCEEDED(hr) && ppDev && *ppDev)
        HookPSCreation(*ppDev);
    return hr;
}

static HRESULT WINAPI HookD3D11CreateDASC(
    IDXGIAdapter* pA, D3D_DRIVER_TYPE dt, HMODULE sw, UINT fl,
    const D3D_FEATURE_LEVEL* pFL, UINT nFL, UINT sdk,
    const DXGI_SWAP_CHAIN_DESC* pSD, IDXGISwapChain** ppSC,
    ID3D11Device** ppDev, D3D_FEATURE_LEVEL* pFLOut, ID3D11DeviceContext** ppCtx)
{
    HRESULT hr = g_origD3D11CreateDASC(pA, dt, sw, fl, pFL, nFL, sdk, pSD, ppSC,
                                        ppDev, pFLOut, ppCtx);
    if (SUCCEEDED(hr) && ppDev && *ppDev)
        HookPSCreation(*ppDev);
    return hr;
}

static void InstallD3D11Hooks()
{
    // Load D3DCompile from d3dcompiler_47.dll for shader compilation.
    HMODULE hCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (hCompiler) {
        g_pD3DCompile = reinterpret_cast<PFN_D3DCompile>(
            GetProcAddress(hCompiler, "D3DCompile"));
        if (g_pD3DCompile)
            Log("D3DCompile: loaded from d3dcompiler_47.dll");
        else
            Log("D3DCompile: GetProcAddress failed");
    } else {
        Log("D3DCompile: d3dcompiler_47.dll not found");
    }

    // Compile HUD replacement shader.
    CompileHudShader();

    MH_STATUS st;

    st = MH_CreateHookApi(L"d3d11.dll", "D3D11CreateDevice",
                           reinterpret_cast<void*>(HookD3D11CreateDevice),
                           reinterpret_cast<void**>(&g_origD3D11CreateDevice));
    if (st == MH_OK) {
        MH_EnableHook(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "D3D11CreateDevice")));
        Log("MinHook: D3D11CreateDevice hooked");
    } else {
        Logf("MinHook: D3D11CreateDevice hook failed (%d)", st);
    }

    st = MH_CreateHookApi(L"d3d11.dll", "D3D11CreateDeviceAndSwapChain",
                           reinterpret_cast<void*>(HookD3D11CreateDASC),
                           reinterpret_cast<void**>(&g_origD3D11CreateDASC));
    if (st == MH_OK) {
        MH_EnableHook(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "D3D11CreateDeviceAndSwapChain")));
        Log("MinHook: D3D11CreateDeviceAndSwapChain hooked");
    } else {
        Logf("MinHook: D3D11CreateDeviceAndSwapChain hook failed (%d)", st);
    }
}

// ---------------------------------------------------------------------------
// Main patching routine (runs in a background thread)
// ---------------------------------------------------------------------------
static void ApplyPatches()
{
    // -- Resolution ----------------------------------------------------------
    SetProcessDPIAware();
    const int W = GetSystemMetrics(SM_CXSCREEN);
    const int H = GetSystemMetrics(SM_CYSCREEN);
    const double aspect     = (double)W / (double)H;
    constexpr double kStd   = 16.0 / 9.0;  // standard aspect ratio

    Logf("Resolution: %dx%d  aspect=%.6f", W, H, aspect);

    // -- FOV calculation (same formula as magic.py) --------------------------
    // hfov = 2 * atan(aspect * (9/16))
    // This preserves the vertical FOV of a 16:9 screen at 90° horizontal.
    const double fovRad = 2.0 * atan(aspect * (9.0 / 16.0));
    const double fovDeg = fovRad * (180.0 / M_PI);

    // Linear mapping found experimentally by the original author:
    //   fovInt = round(129591 * degrees - 10681633)
    // stored as 3-byte little-endian in the exe.
    const int fovInt = (int)round(129591.0 * fovDeg - 10681633.0);

    Logf("FOV: %.2f deg  int=0x%06X (%d)", fovDeg, (unsigned)fovInt, fovInt);

    // -- HUD shift -----------------------------------------------------------
    // The game calculates positions assuming a 1080p-normalised width.
    // standardised_width = W * (1080 / H)
    // hud_shift = -(standardised_width - 1920) / 3840
    const double stdW     = W * (1080.0 / H);
    const double hudShift = -((stdW - 1920.0) / 3840.0);

    Logf("HUD shift: %.4f", hudShift);

    // -- Locate the game module ----------------------------------------------
    HMODULE hExe        = GetModuleHandleW(nullptr);
    auto*   base        = reinterpret_cast<uint8_t*>(hExe);
    auto*   dos         = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto*   nt          = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const size_t imgSz  = nt->OptionalHeader.SizeOfImage;

    Logf("Module base: %p  image size: 0x%zX", (void*)base, imgSz);

    // ========================================================================
    // Patch 1: Black bar / letterbox removal
    //
    // The sequence starts a function with:
    //   48 81 EC D0 00 00 00   sub  rsp, 0D0h
    //   F6 41 2C 01            test byte [rcx+2Ch], 1
    //
    // Zeroing the immediate (01 -> 00) makes the test always produce zero,
    // so the letterbox branch is never taken.
    // ========================================================================
    {
        static const uint8_t kBBPat[] = {
            0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00,
            0xF6, 0x41, 0x2C, 0x01
        };
        uint8_t* addr = FindPattern(base, imgSz, kBBPat, sizeof(kBBPat));
        if (addr)
        {
            const uint8_t zero = 0x00;
            if (PatchBytes(addr + 10, &zero, 1))
                Log("Black bar patch: OK");
            else
                Log("Black bar patch: VirtualProtect failed");
        }
        else
        {
            Log("Black bar patch: pattern not found "
                "(exe may already be patched on disk - OK if bars are gone)");
        }
    }

    // ========================================================================
    // Patch 2: Field of view
    //
    // The 3-byte little-endian FOV integer sits immediately before the unique
    // context suffix 0x3C 0xD8 0xF5.  Default (90 deg, 16:9) = 35 FA 0E.
    // Only applied when aspect ratio is wider than standard 16:9.
    // ========================================================================
    if (aspect > kStd + 0.01)
    {
        if (fovInt <= 0 || fovInt > 0xFFFFFF)
        {
            Logf("FOV patch: computed int out of range (%d) - skipped", fovInt);
        }
        else
        {
            uint8_t* fovAddr = FindFovLocation(base, imgSz);
            if (fovAddr)
            {
                const uint8_t fovBytes[3] = {
                    (uint8_t)( fovInt        & 0xFF),
                    (uint8_t)((fovInt >>  8) & 0xFF),
                    (uint8_t)((fovInt >> 16) & 0xFF),
                };
                if (PatchBytes(fovAddr, fovBytes, 3))
                    Logf("FOV patch: OK  bytes=%02X %02X %02X",
                         fovBytes[0], fovBytes[1], fovBytes[2]);
                else
                    Log("FOV patch: VirtualProtect failed");
            }
            else
            {
                Log("FOV patch: pattern not found");
            }
        }
    }
    else
    {
        Log("FOV patch: skipped (16:9 or narrower)");
    }

    // ========================================================================
    // Shader file patch: Mods/hudtextfix.ini
    //
    // Updates the add-constant in the ShaderRegex HUD shift rule.
    // Safe to run on every launch - idempotent.
    // ========================================================================
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string root(exePath);
        root = root.substr(0, root.rfind('\\') + 1);

        const std::string hudIni = root + "Mods\\hudtextfix.ini";
        if (GetFileAttributesA(hudIni.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            if (UpdateHudShift(hudIni, hudShift))
                Logf("HUD ini patch: OK  (shift=%.4f)", hudShift);
            else
                Log("HUD ini patch: pattern '${register}, l(' not found in file");
        }
        else
        {
            Log("HUD ini patch: Mods/hudtextfix.ini not found - skipped");
        }
    }

    Log("Done.");
}

// ---------------------------------------------------------------------------
// DLL entry point
// ---------------------------------------------------------------------------

static DWORD WINAPI PatchThread(LPVOID)
{
    // Brief delay so the game's loader finishes mapping before we scan.
    Sleep(100);
    ApplyPatches();
    g_log.flush();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hMod);

        // Open log file next to the exe.
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string logPath(exePath);
        logPath = logPath.substr(0, logPath.rfind('\\') + 1) + "ac7ultrawide.log";
        g_log.open(logPath, std::ios::out | std::ios::trunc);
        Log("ac7ultrawide ASI loaded");

        // Compute HUD shift for shader correction.
        {
            SetProcessDPIAware();
            const int w = GetSystemMetrics(SM_CXSCREEN);
            const int h = GetSystemMetrics(SM_CYSCREEN);
            const double aspect = (h > 0) ? (double)w / h : 0.0;
            constexpr double kStd = 16.0 / 9.0;
            if (aspect > kStd + 0.01) {
                const double stdW = w * (1080.0 / h);
                g_hudShift = (float)(-((stdW - 1920.0) / 3840.0));
                Logf("HUD shader: aspect=%.4f shift=%.6f", aspect, g_hudShift);
            }
        }

        // Initialize MinHook and install D3D11 function hooks.
        if (MH_Initialize() == MH_OK) {
            InstallD3D11Hooks();
            Log("MinHook initialized");
        } else {
            Log("MinHook: MH_Initialize failed");
        }

        CloseHandle(CreateThread(nullptr, 0, PatchThread, nullptr, 0, nullptr));
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    return TRUE;
}
