// ac7ultrawide.asi
// Runtime memory patcher for ACE COMBAT 7 ultrawide support.
// Loaded via Ultimate ASI Loader (version.dll in game root).
//
// Replaces magic.py's disk-based exe patching with in-memory patching:
//   - Removes letterbox black bars
//   - Sets horizontal FOV to match your ultrawide aspect ratio
//   - Updates Mods/hudtextfix.ini with the correct HUD shift value
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
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>

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
// D3D11 shader hook — HUD text vertex shader
//
// The HUD-text vertex shader (3Dmigoto FNV-1 hash da86a094e768f000) positions
// text elements assuming 16:9.  On ultrawide the text drifts off-centre.
// We IAT-hook D3D11CreateDevice* at DllMain time, then vtable-hook
// ID3D11Device::CreateVertexShader (slot 12).  When the target shader is
// submitted we compile and return a replacement with the correct clip-space
// x-shift so subtitles, radio text, and map labels stay centred.
//
// Shift formula:  clip_shift = 1 - (16/9) / aspect
// Derivation: 3Dmigoto used 0.2558 for 3440×1440 (aspect 2.3889),
//             this formula gives 0.2559 — matching to 4 significant figures.
// ============================================================================

// Clip-space x-shift for HUD text.  0 on 16:9 (no-op).
// Computed in DllMain before PatchIAT is called.
static double g_clip_shift = 0.0;

// FNV-1 64-bit — 3Dmigoto's default shader hash algorithm.
static uint64_t Fnv1Hash64(const void* data, size_t n)
{
    uint64_t h = 14695981039346656037ULL;
    for (const auto* p = static_cast<const uint8_t*>(data); n--; ++p)
        { h *= 1099511628211ULL; h ^= *p; }
    return h;
}
static constexpr uint64_t kHudTextVsHash = 0xda86a094e768f000ULL;

// Replacement HLSL.  %.6f is substituted with g_clip_shift.
// Logic is identical to the original shader; IniParams (3Dmigoto-only) is
// removed and the shift is applied unconditionally.
static const char kHudVsTemplate[] =
    "cbuffer cb0:register(b0){float4 cb0[4];}\n"
    "void main(\n"
    "  float4 v0:ATTRIBUTE0,float2 v1:ATTRIBUTE1,float2 v2:ATTRIBUTE2,\n"
    "  float4 v3:ATTRIBUTE3,uint2 v4:ATTRIBUTE4,\n"
    "  out float4 o0:SV_POSITION0,out float4 o1:COLOR0,\n"
    "  out float4 o2:ORIGINAL_POSITION0,\n"
    "  out float4 o3:TEXCOORD0,out float4 o4:TEXCOORD1)\n"
    "{\n"
    "  float4 r0=cb0[1]*v2.y; r0=v2.x*cb0[0]+r0; o0=cb0[3]+r0;\n"
    "  float3 c=max((float3)6.10352e-5,v3.xyz);\n"
    "  float3 lo=c*0.0773993805;\n"
    "  float3 hi=exp2(log2(c*0.947867274+0.0521326996)*2.4000001);\n"
    "  o1.xyz=c>(float3)0.0404499993?hi:lo; o1.w=v3.w;\n"
    "  o2.xy=v2.xy; o2.zw=float2(0,1); o3.xy=v1.xy; o4=v0;\n"
    "  o0.x+=%.6f;\n"   // ultrawide clip-space shift
    "}\n";

// D3DCompile loaded lazily from the system d3dcompiler DLL.
typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
static PFN_D3DCompile g_D3DCompile = nullptr;

static bool LoadD3DCompiler()
{
    if (g_D3DCompile) return true;
    static const char* const kDlls[] =
        { "d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll" };
    for (auto* name : kDlls) {
        HMODULE h = LoadLibraryA(name);
        if (!h) continue;
        g_D3DCompile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(h, "D3DCompile"));
        if (g_D3DCompile) return true;
    }
    return false;
}

static bool CompileHudVs(ID3DBlob** ppOut)
{
    if (!LoadD3DCompiler()) { Log("HUD VS: D3DCompiler not found"); return false; }
    char src[2048];
    snprintf(src, sizeof(src), kHudVsTemplate, g_clip_shift);
    ID3DBlob* err = nullptr;
    HRESULT hr = g_D3DCompile(src, strlen(src), "ac7ultrawide_hudvs",
                               nullptr, nullptr, "main", "vs_5_0", 0, 0, ppOut, &err);
    if (err) {
        Logf("HUD VS compile error: %s",
             static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }
    return SUCCEEDED(hr);
}

// ID3D11Device vtable slot 12 = CreateVertexShader.
typedef HRESULT (STDMETHODCALLTYPE *PFN_CVS)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
static PFN_CVS g_origCVS = nullptr;

static HRESULT STDMETHODCALLTYPE HookCreateVertexShader(
    ID3D11Device*       device,
    const void*         pBytecode,
    SIZE_T              len,
    ID3D11ClassLinkage* pCL,
    ID3D11VertexShader** ppVS)
{
    if (g_clip_shift != 0.0 && Fnv1Hash64(pBytecode, len) == kHudTextVsHash) {
        ID3DBlob* blob = nullptr;
        if (CompileHudVs(&blob)) {
            HRESULT hr = g_origCVS(device,
                blob->GetBufferPointer(), blob->GetBufferSize(), pCL, ppVS);
            blob->Release();
            if (SUCCEEDED(hr)) { Log("HUD text VS: patched OK"); return hr; }
            Log("HUD text VS: device rejected replacement, using original");
        }
    }
    return g_origCVS(device, pBytecode, len, pCL, ppVS);
}

static bool g_deviceHooked = false;
static void HookDevice(ID3D11Device* dev)
{
    if (g_deviceHooked) return;
    g_deviceHooked = true;
    void** vt = *reinterpret_cast<void***>(dev);
    DWORD old;
    VirtualProtect(vt + 12, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    g_origCVS = reinterpret_cast<PFN_CVS>(vt[12]);
    vt[12]    = reinterpret_cast<void*>(HookCreateVertexShader);
    VirtualProtect(vt + 12, sizeof(void*), old, &old);
    Log("D3D11 device vtable hooked (slot 12 = CreateVertexShader)");
}

// IAT hook — D3D11CreateDevice.
typedef HRESULT (WINAPI *PFN_CreateDevice)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static PFN_CreateDevice g_origCreateDevice = nullptr;

static HRESULT WINAPI HookD3D11CreateDevice(
    IDXGIAdapter* pA, D3D_DRIVER_TYPE dt, HMODULE sw, UINT fl,
    const D3D_FEATURE_LEVEL* pFL, UINT nFL, UINT sdk,
    ID3D11Device** ppDev, D3D_FEATURE_LEVEL* pFLOut, ID3D11DeviceContext** ppCtx)
{
    HRESULT hr = g_origCreateDevice(pA, dt, sw, fl, pFL, nFL, sdk,
                                    ppDev, pFLOut, ppCtx);
    if (SUCCEEDED(hr) && ppDev && *ppDev) HookDevice(*ppDev);
    return hr;
}

// IAT hook — D3D11CreateDeviceAndSwapChain.
typedef HRESULT (WINAPI *PFN_CreateDASC)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*,
    IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static PFN_CreateDASC g_origCreateDASC = nullptr;

static HRESULT WINAPI HookD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pA, D3D_DRIVER_TYPE dt, HMODULE sw, UINT fl,
    const D3D_FEATURE_LEVEL* pFL, UINT nFL, UINT sdk,
    const DXGI_SWAP_CHAIN_DESC* pSD, IDXGISwapChain** ppSC,
    ID3D11Device** ppDev, D3D_FEATURE_LEVEL* pFLOut, ID3D11DeviceContext** ppCtx)
{
    HRESULT hr = g_origCreateDASC(pA, dt, sw, fl, pFL, nFL, sdk, pSD, ppSC,
                                  ppDev, pFLOut, ppCtx);
    if (SUCCEEDED(hr) && ppDev && *ppDev) HookDevice(*ppDev);
    return hr;
}

static void PatchIAT()
{
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto& dir  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);

    for (; desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), "d3d11.dll") != 0)
            continue;
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        auto* orig  = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
        for (; thunk->u1.Function; ++thunk, ++orig) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + orig->u1.AddressOfData);
            DWORD old;
            if (strcmp(ibn->Name, "D3D11CreateDevice") == 0) {
                VirtualProtect(&thunk->u1.Function, 8, PAGE_EXECUTE_READWRITE, &old);
                g_origCreateDevice = reinterpret_cast<PFN_CreateDevice>(thunk->u1.Function);
                thunk->u1.Function = reinterpret_cast<ULONGLONG>(HookD3D11CreateDevice);
                VirtualProtect(&thunk->u1.Function, 8, old, &old);
                Log("IAT: D3D11CreateDevice hooked");
            } else if (strcmp(ibn->Name, "D3D11CreateDeviceAndSwapChain") == 0) {
                VirtualProtect(&thunk->u1.Function, 8, PAGE_EXECUTE_READWRITE, &old);
                g_origCreateDASC   = reinterpret_cast<PFN_CreateDASC>(thunk->u1.Function);
                thunk->u1.Function = reinterpret_cast<ULONGLONG>(HookD3D11CreateDeviceAndSwapChain);
                VirtualProtect(&thunk->u1.Function, 8, old, &old);
                Log("IAT: D3D11CreateDeviceAndSwapChain hooked");
            }
        }
        break;
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

        // Compute clip-space shift for HUD text VS hook.
        // Done here (not in the patch thread) so g_clip_shift is ready before
        // the game calls D3D11CreateDevice.
        {
            SetProcessDPIAware();
            const int w = GetSystemMetrics(SM_CXSCREEN);
            const int h = GetSystemMetrics(SM_CYSCREEN);
            constexpr double kStd = 16.0 / 9.0;
            if (h > 0) {
                const double aspect = static_cast<double>(w) / h;
                if (aspect > kStd + 0.01)
                    g_clip_shift = 1.0 - kStd / aspect;
            }
            Logf("Shader hook: clip_shift=%.6f", g_clip_shift);
        }
        PatchIAT();

        CloseHandle(CreateThread(nullptr, 0, PatchThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
