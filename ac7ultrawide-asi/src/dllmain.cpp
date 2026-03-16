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
#include <dxgi.h>
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
// One blob per known HUD shader variant, keyed by original bytecode length.
static ID3DBlob* g_compiledHudPS_1104 = nullptr;  // PS[436]: primary HUD (cb1, 2 tex)
static ID3DBlob* g_compiledHudPS_660  = nullptr;  // Simple HUD (cb0, 2 tex)
static ID3DBlob* g_compiledHudPS_2896 = nullptr;  // Complex HUD (cb0+cb1, 5 tex)
static ID3DBlob* g_compiledHudPS_3176 = nullptr;  // Complex HUD + extra bounds (cb0+cb1, 5 tex)
static ID3DBlob* g_compiledHudPS_2936 = nullptr;  // Complex HUD + cb0[20] only (cb0+cb1, 5 tex)
static ID3DBlob* g_compiledHudVS      = nullptr;  // Text/subtitle vertex shader

// VS shift in clip-space.  1 - (16/9)/aspect.  Moves text right to center.
static float g_vsShift = 0.0f;

// Draw-time VS swapping: the CB0[4] text VS is shared between subtitles and
// menus.  An unconditional shift at creation time breaks menus.  We keep both
// the original and shifted VS, and at draw time check:
//   1. Is the current VS our text VS?
//   2. Have any HUD PS been bound this frame? (gameplay detection)
//   3. Is the current PS NOT one of our HUD PS? (avoid double-correction)
// Only when all three are true do we swap in the shifted VS for that draw call.
static ID3D11VertexShader* g_origTextVS    = nullptr;  // original game VS (CB0[4])
static ID3D11VertexShader* g_shiftedTextVS = nullptr;  // our shifted replacement

// Track up to 8 HUD PS replacement pointers for gameplay detection.
static constexpr int kMaxHudPS = 8;
static ID3D11PixelShader* g_hudPSList[kMaxHudPS] = {};
static int g_hudPSCount = 0;

// Per-frame gameplay detection: set true when any HUD PS is bound.
static bool g_hudPSActive = false;

// Track currently-bound VS and PS for draw-time decisions.
static ID3D11VertexShader* g_currentVS = nullptr;
static ID3D11PixelShader*  g_currentPS = nullptr;

// Context hook function pointers.
typedef void (STDMETHODCALLTYPE *PFN_VSSetShader)(
    ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance*const*, UINT);
static PFN_VSSetShader g_origVSSetShader = nullptr;

typedef void (STDMETHODCALLTYPE *PFN_PSSetShader)(
    ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance*const*, UINT);
static PFN_PSSetShader g_origPSSetShader = nullptr;

typedef void (STDMETHODCALLTYPE *PFN_DrawIndexed)(
    ID3D11DeviceContext*, UINT, UINT, INT);
static PFN_DrawIndexed g_origDrawIndexed = nullptr;

typedef void (STDMETHODCALLTYPE *PFN_Draw)(
    ID3D11DeviceContext*, UINT, UINT);
static PFN_Draw g_origDraw = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(
    IDXGISwapChain*, UINT, UINT);
static PFN_Present g_origPresent = nullptr;

// D3DCompile / D3DDisassemble function pointers (from d3dcompiler_47.dll).
typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
    ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
    ID3DBlob**, ID3DBlob**);
static PFN_D3DCompile g_pD3DCompile = nullptr;

typedef HRESULT (WINAPI *PFN_D3DDisassemble)(
    LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
static PFN_D3DDisassemble g_pD3DDisassemble = nullptr;

// Game root directory (for writing diagnostic dumps).
static std::string g_gameRoot;

// Shader hook function pointers.
typedef HRESULT (STDMETHODCALLTYPE *PFN_CPS)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
static PFN_CPS g_origCPS = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *PFN_CVS)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
static PFN_CVS g_origCVS = nullptr;

// ---------------------------------------------------------------------------
// HLSL templates for HUD pixel shaders.
// Each template has a %.6f placeholder for the HUD shift value.
// Derived from D3DDisassemble output using the #define cmp - trick.
// ---------------------------------------------------------------------------

// PS len=1104: primary HUD shader (cb1[127], 2 textures, 1 sampler).
// Based on 3Dmigoto's proven 9958a636cbef5557-ps_replace.txt.
static const char kHudPS_1104_Fmt[] =
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

// PS len=660: simple HUD shader (cb0[127], 2 textures, 1 sampler).
static const char kHudPS_660_Fmt[] =
    "cbuffer cb0 : register(b0) { float4 cb0[129]; }\n"
    "Texture2D<float4> t1 : register(t1);\n"
    "Texture2D<float4> t0 : register(t0);\n"
    "SamplerState s0_s : register(s0);\n"
    "#define cmp -\n"
    "void main(float2 v0 : TEXCOORD0, out float4 o0 : SV_Target0) {\n"
    "  float4 r0,r1,r2;\n"
    "  r0.x = cmp(1 < cb0[127].y);\n"
    "  r0.y = 1.777778 * cb0[127].y;\n"
    "  r0.y += %.6f;\n"
    "  r0.y = cb0[127].x / r0.y;\n"
    "  r0.zw = v0.xy * cb0[128].xy + -cb0[126].xy;\n"
    "  r1.yz = cb0[127].zw * r0.zw;\n"
    "  r0.y = r1.y * r0.y;\n"
    "  r1.x = r0.x ? r0.y : r1.y;\n"
    "  r0.xyzw = t1.Sample(s0_s, r1.xz).xyzw;\n"
    "  r1.xyzw = t0.Sample(s0_s, v0.xy).xyzw;\n"
    "  r2.x = 1 + -r1.w;\n"
    "  o0.xyzw = r2.xxxx * r0.xyzw + r1.xyzw;\n"
    "}\n";

// PS len=2896: complex HUD shader (cb0[26]+cb1[129], 5 textures, 5 samplers).
static const char kHudPS_2896_Fmt[] =
    "cbuffer cb0 : register(b0) { float4 cb0[26]; }\n"
    "cbuffer cb1 : register(b1) { float4 cb1[129]; }\n"
    "Texture2D<float4> t0 : register(t0);\n"
    "Texture2D<float4> t1 : register(t1);\n"
    "Texture2D<float4> t2 : register(t2);\n"
    "Texture2D<float4> t3 : register(t3);\n"
    "Texture2D<float4> t4 : register(t4);\n"
    "SamplerState s0_s : register(s0);\n"
    "SamplerState s1_s : register(s1);\n"
    "SamplerState s2_s : register(s2);\n"
    "SamplerState s3_s : register(s3);\n"
    "SamplerState s4_s : register(s4);\n"
    "#define cmp -\n"
    "void main(float2 v0 : TEXCOORD0, out float4 o0 : SV_Target0) {\n"
    "  float4 r0,r1,r2,r3,r4,r5,r6,r7;\n"
    "  r0.xy = v0.xy * cb1[128].xy + -cb1[126].xy;\n"
    "  r0.xy = r0.xy * cb1[127].zw;\n"
    "  r0.z = cmp(1 < cb1[127].y);\n"
    "  r0.w = 1.777778 * cb1[127].y;\n"
    "  r0.w += %.6f;\n"
    "  r0.w = cb1[127].x / r0.w;\n"
    "  r0.w = r0.w * r0.x;\n"
    "  r0.x = r0.z ? r0.w : r0.x;\n"
    "  r0.z = cmp(0.5 < cb0[24].y);\n"
    "  r0.w = cmp(r0.x >= cb0[19].x);\n"
    "  r0.x = cmp(cb0[19].z >= r0.x);\n"
    "  r0.x = (int)r0.x & (int)r0.w;\n"
    "  r0.w = cmp(r0.y >= cb0[19].y);\n"
    "  r0.x = (int)r0.w & (int)r0.x;\n"
    "  r0.y = cmp(cb0[19].w >= r0.y);\n"
    "  r0.x = (int)r0.y & (int)r0.x;\n"
    "  r0.x = (int)r0.x | (int)r0.z;\n"
    "  r1.xyzw = t0.Sample(s0_s, v0.xy).xyzw;\n"
    "  r2.xyzw = t1.Sample(s1_s, v0.xy).xyzw;\n"
    "  r0.y = dot(r1.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "  r0.y = saturate(1 + -r0.y);\n"
    "  r0.y = r0.x ? 1.0 : r0.y;\n"
    "  r0.yz = r0.yy * cb0[18].xy;\n"
    "  r0.w = cmp(-0.01 < cb0[23].x);\n"
    "  r3.xy = r0.yz * cb0[23].xx;\n"
    "  r0.yz = r0.w ? r3.xy : r0.yz;\n"
    "  r3.xyz = t2.Sample(s2_s, v0.xy).xyz;\n"
    "  r4.xyz = r0.yyy * r3.xyz;\n"
    "  r0.w = saturate(dot(r4.xyz, float3(0.298912, 0.586611, 0.114477)));\n"
    "  r2.xyz = r3.xyz * r0.yyy + r2.xyz;\n"
    "  r0.y = (1 - r2.w) * r0.w + r2.w;\n"
    "  r1.xyz = r1.xyz * cb0[24].xxx + cb0[23].zzz;\n"
    "  r0.w = cb1[127].y / cb1[127].x;\n"
    "  r3.y = r0.w * v0.y;\n"
    "  r3.x = v0.x;\n"
    "  r3.xy = r3.xy * float2(40.0, 40.0);\n"
    "  r0.w = t4.Sample(s4_s, r3.xy).x;\n"
    "  r3.xyz = t3.Sample(s3_s, v0.xy).xyz;\n"
    "  r3.xyz = r0.zzz * r3.xyz;\n"
    "  r0.z = dot(r3.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "  r4.xyz = r0.www * r3.xyz;\n"
    "  r4.xyz = r4.xyz * float3(3.0, 3.0, 3.0);\n"
    "  r0.z = saturate(r0.z + r0.z);\n"
    "  r3.xyz = r4.xyz * r0.zzz + r3.xyz;\n"
    "  r4.xw = cb1[128].zw * float2(2.0, 2.0);\n"
    "  r4.yz = float2(0, 0);\n"
    "  r5.xyzw = v0.xyxy - r4.xyzw;\n"
    "  r6.xyz = t1.Sample(s1_s, r5.xy).xyz;\n"
    "  r4.xyzw = v0.xyxy + r4.xyzw;\n"
    "  r7.xyz = t1.Sample(s1_s, r4.xy).xyz;\n"
    "  r5.xyz = t1.Sample(s1_s, r5.zw).xyz;\n"
    "  r4.xyz = t1.Sample(s1_s, r4.zw).xyz;\n"
    "  if (r0.x) {\n"
    "    r0.x = 1 - r0.y;\n"
    "    r0.xzw = r1.xyz * r0.xxx + r2.xyz;\n"
    "    r2.w = saturate(dot(r3.xyz, float3(0.298912, 0.586611, 0.114477)));\n"
    "    r2.w = 1 - r2.w;\n"
    "    r0.xzw = r0.xzw * r2.www + r3.xyz;\n"
    "  } else {\n"
    "    r2.w = dot(r6.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "    r3.w = dot(r7.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "    r2.w = r2.w * r3.w;\n"
    "    r3.w = dot(r5.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "    r2.w = r2.w * r3.w;\n"
    "    r3.w = dot(r4.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "    r2.w = r2.w * r3.w;\n"
    "    r0.y = 1 + -r0.y * cb0[25].x;\n"
    "    r2.xyz = r2.xyz * cb0[23].www;\n"
    "    r2.w = r2.w * 10.0 + 1.0;\n"
    "    r2.xyz = r2.www * r2.xyz;\n"
    "    r1.xyz = r1.xyz * r0.yyy + r2.xyz;\n"
    "    r0.xzw = r3.xyz + r1.xyz;\n"
    "  }\n"
    "  o0.xyz = r0.xzw * cb0[23].yyy;\n"
    "  o0.w = r1.w;\n"
    "}\n";

// PS len=3176: complex HUD + extra bounds check (cb0[26]+cb1[129], 5 tex, 5 samp).
// Nearly identical to len=2896 but adds cb0[20] region test merged with cb0[19].
static const char kHudPS_3176_Fmt[] =
    "cbuffer cb0 : register(b0) { float4 cb0[26]; }\n"
    "cbuffer cb1 : register(b1) { float4 cb1[129]; }\n"
    "Texture2D<float4> t0 : register(t0);\n"
    "Texture2D<float4> t1 : register(t1);\n"
    "Texture2D<float4> t2 : register(t2);\n"
    "Texture2D<float4> t3 : register(t3);\n"
    "Texture2D<float4> t4 : register(t4);\n"
    "SamplerState s0_s : register(s0);\n"
    "SamplerState s1_s : register(s1);\n"
    "SamplerState s2_s : register(s2);\n"
    "SamplerState s3_s : register(s3);\n"
    "SamplerState s4_s : register(s4);\n"
    "#define cmp -\n"
    "void main(float2 v0 : TEXCOORD0, out float4 o0 : SV_Target0) {\n"
    "  float4 r0,r1,r2,r3,r4,r5,r6,r7;\n"
    "  r0.xy = v0.xy * cb1[128].xy + -cb1[126].xy;\n"
    "  r0.xy = r0.xy * cb1[127].zw;\n"
    "  r0.z = cmp(1 < cb1[127].y);\n"
    "  r0.w = 1.777778 * cb1[127].y;\n"
    "  r0.w += %.6f;\n"
    "  r0.w = cb1[127].x / r0.w;\n"
    "  r0.w = r0.w * r0.x;\n"
    "  r0.x = r0.z ? r0.w : r0.x;\n"
    "  r0.z = cmp(0.5 < cb0[24].y);\n"
    "  r0.w = cmp(r0.x >= cb0[19].x);\n"
    "  r1.x = cmp(cb0[19].z >= r0.x);\n"
    "  r0.w = (int)r0.w & (int)r1.x;\n"
    "  r1.x = cmp(r0.y >= cb0[19].y);\n"
    "  r0.w = (int)r0.w & (int)r1.x;\n"
    "  r1.x = cmp(cb0[19].w >= r0.y);\n"
    "  r0.w = (int)r0.w & (int)r1.x;\n"
    "  r0.z = (int)r0.w | (int)r0.z;\n"
    "  r1.xyzw = t0.Sample(s0_s, v0.xy).xyzw;\n"
    "  r2.xyzw = t1.Sample(s1_s, v0.xy).xyzw;\n"
    "  r0.w = dot(r1.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "  r0.w = saturate(1 + -r0.w);\n"
    "  r3.x = cmp(r0.x >= cb0[20].x);\n"
    "  r0.x = cmp(cb0[20].z >= r0.x);\n"
    "  r0.x = (int)r0.x & (int)r3.x;\n"
    "  r3.x = cmp(r0.y >= cb0[20].y);\n"
    "  r0.x = (int)r0.x & (int)r3.x;\n"
    "  r0.y = cmp(cb0[20].w >= r0.y);\n"
    "  r0.x = (int)r0.y & (int)r0.x;\n"
    "  r0.y = r0.x ? cb0[18].z : r0.w;\n"
    "  r0.y = r0.z ? 1.0 : r0.y;\n"
    "  r0.yw = r0.yy * cb0[18].xy;\n"
    "  r3.x = cmp(-0.01 < cb0[23].x);\n"
    "  r3.yz = r0.yw * cb0[23].xx;\n"
    "  r0.yw = r3.x ? r3.yz : r0.yw;\n"
    "  r3.xyz = t2.Sample(s2_s, v0.xy).xyz;\n"
    "  r4.xyz = r0.yyy * r3.xyz;\n"
    "  r3.w = saturate(dot(r4.xyz, float3(0.298912, 0.586611, 0.114477)));\n"
    "  r2.xyz = r3.xyz * r0.yyy + r2.xyz;\n"
    "  r0.y = (1 - r2.w) * r3.w + r2.w;\n"
    "  r1.xyz = r1.xyz * cb0[24].xxx + cb0[23].zzz;\n"
    "  r2.w = cb1[127].y / cb1[127].x;\n"
    "  r3.y = r2.w * v0.y;\n"
    "  r3.x = v0.x;\n"
    "  r3.xy = r3.xy * float2(40.0, 40.0);\n"
    "  r2.w = t4.Sample(s4_s, r3.xy).x;\n"
    "  r3.xyz = t3.Sample(s3_s, v0.xy).xyz;\n"
    "  r3.xyz = r0.www * r3.xyz;\n"
    "  r0.w = dot(r3.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "  r4.xyz = r2.www * r3.xyz;\n"
    "  r4.xyz = r4.xyz * float3(3.0, 3.0, 3.0);\n"
    "  r0.w = saturate(r0.w + r0.w);\n"
    "  r3.xyz = r4.xyz * r0.www + r3.xyz;\n"
    "  r0.x = (int)r0.x | (int)r0.z;\n"
    "  r4.xw = cb1[128].zw * float2(2.0, 2.0);\n"
    "  r4.yz = float2(0, 0);\n"
    "  r5.xyzw = v0.xyxy - r4.xyzw;\n"
    "  r6.xyz = t1.Sample(s1_s, r5.xy).xyz;\n"
    "  r4.xyzw = v0.xyxy + r4.xyzw;\n"
    "  r7.xyz = t1.Sample(s1_s, r4.xy).xyz;\n"
    "  r5.xyz = t1.Sample(s1_s, r5.zw).xyz;\n"
    "  r4.xyz = t1.Sample(s1_s, r4.zw).xyz;\n"
    "  if (r0.x) {\n"
    "    r0.x = 1 - r0.y;\n"
    "    r0.xzw = r1.xyz * r0.xxx + r2.xyz;\n"
    "    r2.w = saturate(dot(r3.xyz, float3(0.298912, 0.586611, 0.114477)));\n"
    "    r2.w = 1 - r2.w;\n"
    "    r0.xzw = r0.xzw * r2.www + r3.xyz;\n"
    "  } else {\n"
    "    r2.w = dot(r6.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "    r3.w = dot(r7.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "    r2.w = r2.w * r3.w;\n"
    "    r3.w = dot(r5.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "    r2.w = r2.w * r3.w;\n"
    "    r3.w = dot(r4.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "    r2.w = r2.w * r3.w;\n"
    "    r0.y = 1 + -r0.y * cb0[25].x;\n"
    "    r2.xyz = r2.xyz * cb0[23].www;\n"
    "    r2.w = r2.w * 10.0 + 1.0;\n"
    "    r2.xyz = r2.www * r2.xyz;\n"
    "    r1.xyz = r1.xyz * r0.yyy + r2.xyz;\n"
    "    r0.xzw = r3.xyz + r1.xyz;\n"
    "  }\n"
    "  o0.xyz = r0.xzw * cb0[23].yyy;\n"
    "  o0.w = r1.w;\n"
    "}\n";

// PS len=2936: complex HUD + cb0[20] only, no cb0[19] (cb0[26]+cb1[129], 5 tex, 5 samp).
static const char kHudPS_2936_Fmt[] =
    "cbuffer cb0 : register(b0) { float4 cb0[26]; }\n"
    "cbuffer cb1 : register(b1) { float4 cb1[129]; }\n"
    "Texture2D<float4> t0 : register(t0);\n"
    "Texture2D<float4> t1 : register(t1);\n"
    "Texture2D<float4> t2 : register(t2);\n"
    "Texture2D<float4> t3 : register(t3);\n"
    "Texture2D<float4> t4 : register(t4);\n"
    "SamplerState s0_s : register(s0);\n"
    "SamplerState s1_s : register(s1);\n"
    "SamplerState s2_s : register(s2);\n"
    "SamplerState s3_s : register(s3);\n"
    "SamplerState s4_s : register(s4);\n"
    "#define cmp -\n"
    "void main(float2 v0 : TEXCOORD0, out float4 o0 : SV_Target0) {\n"
    "  float4 r0,r1,r2,r3,r4,r5,r6,r7;\n"
    "  r0.xy = v0.xy * cb1[128].xy + -cb1[126].xy;\n"
    "  r0.xy = r0.xy * cb1[127].zw;\n"
    "  r0.z = cmp(1 < cb1[127].y);\n"
    "  r0.w = 1.777778 * cb1[127].y;\n"
    "  r0.w += %.6f;\n"
    "  r0.w = cb1[127].x / r0.w;\n"
    "  r0.w = r0.w * r0.x;\n"
    "  r0.x = r0.z ? r0.w : r0.x;\n"
    "  r0.z = cmp(0.5 < cb0[24].y);\n"
    "  r1.xyzw = t0.Sample(s0_s, v0.xy).xyzw;\n"
    "  r2.xyzw = t1.Sample(s1_s, v0.xy).xyzw;\n"
    "  r0.w = dot(r1.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "  r0.w = saturate(1 + -r0.w);\n"
    "  r3.x = cmp(r0.x >= cb0[20].x);\n"
    "  r0.x = cmp(cb0[20].z >= r0.x);\n"
    "  r0.x = (int)r0.x & (int)r3.x;\n"
    "  r3.x = cmp(r0.y >= cb0[20].y);\n"
    "  r0.x = (int)r0.x & (int)r3.x;\n"
    "  r0.y = cmp(cb0[20].w >= r0.y);\n"
    "  r0.x = (int)r0.y & (int)r0.x;\n"
    "  r0.y = r0.x ? cb0[18].z : r0.w;\n"
    "  r0.y = r0.z ? 1.0 : r0.y;\n"
    "  r0.yw = r0.yy * cb0[18].xy;\n"
    "  r3.x = cmp(-0.01 < cb0[23].x);\n"
    "  r3.yz = r0.yw * cb0[23].xx;\n"
    "  r0.yw = r3.x ? r3.yz : r0.yw;\n"
    "  r3.xyz = t2.Sample(s2_s, v0.xy).xyz;\n"
    "  r4.xyz = r0.yyy * r3.xyz;\n"
    "  r3.w = saturate(dot(r4.xyz, float3(0.298912, 0.586611, 0.114477)));\n"
    "  r2.xyz = r3.xyz * r0.yyy + r2.xyz;\n"
    "  r0.y = (1 - r2.w) * r3.w + r2.w;\n"
    "  r1.xyz = r1.xyz * cb0[24].xxx + cb0[23].zzz;\n"
    "  r2.w = cb1[127].y / cb1[127].x;\n"
    "  r3.y = r2.w * v0.y;\n"
    "  r3.x = v0.x;\n"
    "  r3.xy = r3.xy * float2(40.0, 40.0);\n"
    "  r2.w = t4.Sample(s4_s, r3.xy).x;\n"
    "  r3.xyz = t3.Sample(s3_s, v0.xy).xyz;\n"
    "  r3.xyz = r0.www * r3.xyz;\n"
    "  r0.w = dot(r3.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "  r4.xyz = r2.www * r3.xyz;\n"
    "  r4.xyz = r4.xyz * float3(3.0, 3.0, 3.0);\n"
    "  r0.w = saturate(r0.w + r0.w);\n"
    "  r3.xyz = r4.xyz * r0.www + r3.xyz;\n"
    "  r0.x = (int)r0.x | (int)r0.z;\n"
    "  r4.xw = cb1[128].zw * float2(2.0, 2.0);\n"
    "  r4.yz = float2(0, 0);\n"
    "  r5.xyzw = v0.xyxy - r4.xyzw;\n"
    "  r6.xyz = t1.Sample(s1_s, r5.xy).xyz;\n"
    "  r4.xyzw = v0.xyxy + r4.xyzw;\n"
    "  r7.xyz = t1.Sample(s1_s, r4.xy).xyz;\n"
    "  r5.xyz = t1.Sample(s1_s, r5.zw).xyz;\n"
    "  r4.xyz = t1.Sample(s1_s, r4.zw).xyz;\n"
    "  if (r0.x) {\n"
    "    r0.x = 1 - r0.y;\n"
    "    r0.xzw = r1.xyz * r0.xxx + r2.xyz;\n"
    "    r2.w = saturate(dot(r3.xyz, float3(0.298912, 0.586611, 0.114477)));\n"
    "    r2.w = 1 - r2.w;\n"
    "    r0.xzw = r0.xzw * r2.www + r3.xyz;\n"
    "  } else {\n"
    "    r2.w = dot(r6.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "    r3.w = dot(r7.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "    r2.w = r2.w * r3.w;\n"
    "    r3.w = dot(r5.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "    r2.w = r2.w * r3.w;\n"
    "    r3.w = dot(r4.xyz, float3(0.298912, 0.586611, 0.114477));\n"
    "    r2.w = r2.w * r3.w;\n"
    "    r0.y = 1 + -r0.y * cb0[25].x;\n"
    "    r2.xyz = r2.xyz * cb0[23].www;\n"
    "    r2.w = r2.w * 10.0 + 1.0;\n"
    "    r2.xyz = r2.www * r2.xyz;\n"
    "    r1.xyz = r1.xyz * r0.yyy + r2.xyz;\n"
    "    r0.xzw = r3.xyz + r1.xyz;\n"
    "  }\n"
    "  o0.xyz = r0.xzw * cb0[23].yyy;\n"
    "  o0.w = r1.w;\n"
    "}\n";

// ---------------------------------------------------------------------------
// VS template: HUD text / subtitle vertex shader.
// Identified by sRGB gamma constant 0.947867274.
// Adds a clip-space x offset to shift text right on ultrawide.
// VS shift = 1 - (16/9)/aspect.  Verified: gives 0.5 for 32:9 (matches
// the old 3Dmigoto vs_replace that used o0.x += 0.5 for that aspect ratio).
// ---------------------------------------------------------------------------
static const char kHudVS_Fmt[] =
    "cbuffer cb0 : register(b0) { float4 cb0[4]; }\n"
    "#define cmp -\n"
    "void main(\n"
    "  float4 v0 : ATTRIBUTE0,\n"
    "  float2 v1 : ATTRIBUTE1,\n"
    "  float2 v2 : ATTRIBUTE2,\n"
    "  float4 v3 : ATTRIBUTE3,\n"
    "  uint2  v4 : ATTRIBUTE4,\n"
    "  out float4 o0 : SV_POSITION0,\n"
    "  out float4 o1 : COLOR0,\n"
    "  out float4 o2 : ORIGINAL_POSITION0,\n"
    "  out float4 o3 : TEXCOORD0,\n"
    "  out float4 o4 : TEXCOORD1)\n"
    "{\n"
    "  float4 r0,r1,r2;\n"
    "  r0.xyzw = cb0[1].xyzw * v2.yyyy;\n"
    "  r0.xyzw = v2.xxxx * cb0[0].xyzw + r0.xyzw;\n"
    "  o0.xyzw = cb0[3].xyzw + r0.xyzw;\n"
    "  r0.xyz = max(float3(6.10352e-005,6.10352e-005,6.10352e-005), v3.xyz);\n"
    "  r1.xyz = r0.xyz * float3(0.947867274,0.947867274,0.947867274)"
                      " + float3(0.0521326996,0.0521326996,0.0521326996);\n"
    "  r1.xyz = log2(r1.xyz);\n"
    "  r1.xyz = float3(2.4,2.4,2.4) * r1.xyz;\n"
    "  r1.xyz = exp2(r1.xyz);\n"
    "  r2.xyz = cmp(float3(0.04045,0.04045,0.04045) < r0.xyz);\n"
    "  r0.xyz = float3(0.0773993805,0.0773993805,0.0773993805) * r0.xyz;\n"
    "  o1.xyz = r2.xyz ? r1.xyz : r0.xyz;\n"
    "  o1.w = v3.w;\n"
    "  o2.xy = v2.xy;\n"
    "  o2.zw = float2(0,1);\n"
    "  o3.xy = v1.xy;\n"
    "  o3.zw = float2(0,0);\n"
    "  o4.xyzw = v0.xyzw;\n"
    "  o0.x += %.6f;\n"
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

// Check if bytecode contains the sRGB gamma constant (identifies text VS).
static bool ContainsSRGBGamma(const void* bytecode, SIZE_T len)
{
    const float kTarget = 0.947867274f;
    const auto* bytes = static_cast<const uint8_t*>(bytecode);
    for (SIZE_T i = 0; i + 4 <= len; i += 4) {
        float val;
        memcpy(&val, bytes + i, 4);
        if (fabsf(val - kTarget) < 0.0001f)
            return true;
    }
    return false;
}

// Compile a single HUD shader from an HLSL format template.
static ID3DBlob* CompileOneShader(const char* fmt, float shiftVal,
                                   const char* name, const char* target)
{
    char hlsl[4096];
    snprintf(hlsl, sizeof(hlsl), fmt, shiftVal);

    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = g_pD3DCompile(
        hlsl, strlen(hlsl),
        name, nullptr, nullptr,
        "main", target,
        0, 0,
        &code, &errors);

    if (FAILED(hr)) {
        if (errors) {
            Logf("D3DCompile %s failed: %s", name,
                 (const char*)errors->GetBufferPointer());
            errors->Release();
        } else {
            Logf("D3DCompile %s failed: hr=0x%08X", name, (unsigned)hr);
        }
        return nullptr;
    }
    if (errors) errors->Release();

    Logf("D3DCompile %s: OK (%zu bytes)", name, code->GetBufferSize());
    return code;
}

// Compile all HUD replacement shaders (PS + VS).
static void CompileHudShaders()
{
    if (!g_pD3DCompile) return;

    if (g_hudShift != 0.0f) {
        g_compiledHudPS_1104 = CompileOneShader(kHudPS_1104_Fmt, g_hudShift, "HudPS_1104", "ps_5_0");
        g_compiledHudPS_660  = CompileOneShader(kHudPS_660_Fmt,  g_hudShift, "HudPS_660",  "ps_5_0");
        g_compiledHudPS_2896 = CompileOneShader(kHudPS_2896_Fmt, g_hudShift, "HudPS_2896", "ps_5_0");
        g_compiledHudPS_3176 = CompileOneShader(kHudPS_3176_Fmt, g_hudShift, "HudPS_3176", "ps_5_0");
        g_compiledHudPS_2936 = CompileOneShader(kHudPS_2936_Fmt, g_hudShift, "HudPS_2936", "ps_5_0");
        Logf("HUD PS compilation: shift=%.6f", g_hudShift);
    }

    if (g_vsShift != 0.0f) {
        g_compiledHudVS = CompileOneShader(kHudVS_Fmt, g_vsShift, "HudVS", "vs_5_0");
        Logf("HUD VS compilation: shift=%.6f", g_vsShift);
    }
}

static HRESULT STDMETHODCALLTYPE HookCreatePixelShader(
    ID3D11Device*       device,
    const void*         pBytecode,
    SIZE_T              len,
    ID3D11ClassLinkage* pCL,
    ID3D11PixelShader** ppPS)
{
    // Detect shaders containing the 16/9 aspect literal and substitute.
    if (g_hudShift != 0.0f && ContainsAspectFloat(pBytecode, len)) {
        // Look up compiled replacement by original bytecode length.
        ID3DBlob* replacement = nullptr;
        if      (len == 1104) replacement = g_compiledHudPS_1104;
        else if (len == 660)  replacement = g_compiledHudPS_660;
        else if (len == 2896) replacement = g_compiledHudPS_2896;
        else if (len == 3176) replacement = g_compiledHudPS_3176;
        else if (len == 2936) replacement = g_compiledHudPS_2936;

        if (replacement) {
            HRESULT hr = g_origCPS(device,
                                    replacement->GetBufferPointer(),
                                    replacement->GetBufferSize(),
                                    pCL, ppPS);
            if (SUCCEEDED(hr)) {
                Logf("PS HUD patch: OK (original len=%zu, replacement len=%zu)",
                     len, replacement->GetBufferSize());
                // Track this replacement PS for gameplay detection.
                if (ppPS && *ppPS && g_hudPSCount < kMaxHudPS)
                    g_hudPSList[g_hudPSCount++] = *ppPS;
                g_log.flush();
                return hr;
            }
            Logf("PS HUD patch: CreatePixelShader failed (hr=0x%08X) for len=%zu",
                 (unsigned)hr, len);
            g_log.flush();
        } else {
            Logf("PS HUD candidate: len=%zu contains 16/9 float (no replacement yet)",
                 len);
            // Dump disassembly to file for HLSL template creation.
            if (g_pD3DDisassemble && !g_gameRoot.empty()) {
                ID3DBlob* asmBlob = nullptr;
                HRESULT dhr = g_pD3DDisassemble(pBytecode, len, 0, nullptr, &asmBlob);
                if (SUCCEEDED(dhr) && asmBlob) {
                    char fname[256];
                    snprintf(fname, sizeof(fname), "ac7_hud_ps_len%zu.txt", len);
                    std::string path = g_gameRoot + fname;
                    std::ofstream dump(path, std::ios::out | std::ios::trunc);
                    if (dump.is_open()) {
                        dump.write((const char*)asmBlob->GetBufferPointer(),
                                   asmBlob->GetBufferSize());
                        Logf("  -> dumped disassembly to %s", fname);
                    }
                    asmBlob->Release();
                }
            }
            g_log.flush();
        }
    }

    return g_origCPS(device, pBytecode, len, pCL, ppPS);
}

static HRESULT STDMETHODCALLTYPE HookCreateVertexShader(
    ID3D11Device*       device,
    const void*         pBytecode,
    SIZE_T              len,
    ID3D11ClassLinkage* pCL,
    ID3D11VertexShader** ppVS)
{
    // Detect the subtitle/in-game text VS (CB0[4]) vs menu VS (CB0[6]).
    // Both are len=1048 with sRGB gamma constant. They differ at byte offset
    // 0x198 in the DXBC: the dcl_constantbuffer size field.
    if (g_vsShift != 0.0f && g_compiledHudVS &&
        ContainsSRGBGamma(pBytecode, len) && len == 1048) {
        const auto* bytes = static_cast<const uint8_t*>(pBytecode);
        uint32_t cbSize = 0;
        memcpy(&cbSize, bytes + 0x198, 4);

        if (cbSize == 4) {
            // Create the original VS normally first.
            HRESULT hr = g_origCVS(device, pBytecode, len, pCL, ppVS);
            if (SUCCEEDED(hr) && ppVS && *ppVS) {
                g_origTextVS = *ppVS;
                // Now create the shifted replacement VS.
                ID3D11VertexShader* shifted = nullptr;
                HRESULT hr2 = g_origCVS(device,
                                          g_compiledHudVS->GetBufferPointer(),
                                          g_compiledHudVS->GetBufferSize(),
                                          pCL, &shifted);
                if (SUCCEEDED(hr2) && shifted) {
                    g_shiftedTextVS = shifted;
                    Logf("VS HUD: saved original + created shifted replacement "
                         "(original len=%zu, shifted len=%zu)",
                         len, g_compiledHudVS->GetBufferSize());
                } else {
                    Logf("VS HUD: failed to create shifted replacement (hr=0x%08X)",
                         (unsigned)hr2);
                }
                g_log.flush();
            }
            return hr;
        } else {
            Logf("VS sRGB candidate: len=%zu CB0[%u] skipped (not subtitle VS)", len, cbSize);
            g_log.flush();
        }
    }

    return g_origCVS(device, pBytecode, len, pCL, ppVS);
}

// ---------------------------------------------------------------------------
// Device context hooks: draw-time VS swapping + gameplay detection
// ---------------------------------------------------------------------------

// Reset per-frame state at the end of each frame.
static HRESULT STDMETHODCALLTYPE HookPresent(
    IDXGISwapChain* sc, UINT syncInterval, UINT flags)
{
    g_hudPSActive = false;
    return g_origPresent(sc, syncInterval, flags);
}

// Track which VS the game binds (no swapping here).
static void STDMETHODCALLTYPE HookVSSetShader(
    ID3D11DeviceContext* ctx, ID3D11VertexShader* vs,
    ID3D11ClassInstance*const* ppCI, UINT numCI)
{
    g_currentVS = vs;
    g_origVSSetShader(ctx, vs, ppCI, numCI);
}

// Track which PS the game binds; detect HUD PS for gameplay signal.
static void STDMETHODCALLTYPE HookPSSetShader(
    ID3D11DeviceContext* ctx, ID3D11PixelShader* ps,
    ID3D11ClassInstance*const* ppCI, UINT numCI)
{
    g_currentPS = ps;
    if (ps) {
        for (int i = 0; i < g_hudPSCount; ++i) {
            if (g_hudPSList[i] == ps) {
                g_hudPSActive = true;
                break;
            }
        }
    }
    g_origPSSetShader(ctx, ps, ppCI, numCI);
}

// Check if the currently-bound PS is one of our HUD replacements.
static bool IsCurrentPSHud()
{
    for (int i = 0; i < g_hudPSCount; ++i)
        if (g_hudPSList[i] == g_currentPS) return true;
    return false;
}

// At draw time, conditionally swap the text VS to its shifted version.
// Conditions: current VS is our text VS, we're in gameplay (HUD PS was
// bound this frame), and the current PS is NOT a HUD PS (to avoid
// double-correction — HUD PS already has position fix baked in).
static void STDMETHODCALLTYPE HookDrawIndexed(
    ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex)
{
    if (g_currentVS == g_origTextVS && g_shiftedTextVS &&
        g_hudPSActive && !IsCurrentPSHud()) {
        g_origVSSetShader(ctx, g_shiftedTextVS, nullptr, 0);
        g_origDrawIndexed(ctx, indexCount, startIndex, baseVertex);
        g_origVSSetShader(ctx, g_origTextVS, nullptr, 0);
        return;
    }
    g_origDrawIndexed(ctx, indexCount, startIndex, baseVertex);
}

static void STDMETHODCALLTYPE HookDraw(
    ID3D11DeviceContext* ctx, UINT vertexCount, UINT startVertex)
{
    if (g_currentVS == g_origTextVS && g_shiftedTextVS &&
        g_hudPSActive && !IsCurrentPSHud()) {
        g_origVSSetShader(ctx, g_shiftedTextVS, nullptr, 0);
        g_origDraw(ctx, vertexCount, startVertex);
        g_origVSSetShader(ctx, g_origTextVS, nullptr, 0);
        return;
    }
    g_origDraw(ctx, vertexCount, startVertex);
}

// Hook Present on the swap chain via DXGI for per-frame state reset.
// Called from HookShaderCreation (works even when the game uses
// D3D11CreateDevice + separate CreateSwapChain instead of DASC).
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChain)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
static PFN_CreateSwapChain g_origCreateSwapChain = nullptr;

static HRESULT STDMETHODCALLTYPE HookCreateSwapChain(
    IDXGIFactory* factory, IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSC)
{
    HRESULT hr = g_origCreateSwapChain(factory, pDevice, pDesc, ppSC);
    if (SUCCEEDED(hr) && ppSC && *ppSC && !g_origPresent) {
        void** scVtable = *reinterpret_cast<void***>(*ppSC);
        MH_STATUS st = MH_CreateHook(scVtable[8],
                                      reinterpret_cast<void*>(HookPresent),
                                      reinterpret_cast<void**>(&g_origPresent));
        if (st == MH_OK) {
            MH_EnableHook(scVtable[8]);
            Log("MinHook: Present hooked OK (via CreateSwapChain)");
        } else {
            Logf("MinHook: Present hook failed (%d)", st);
        }
    }
    return hr;
}

static void HookDXGIFactory(ID3D11Device* dev)
{
    IDXGIDevice* dxgiDev = nullptr;
    HRESULT hr = dev->QueryInterface(__uuidof(IDXGIDevice),
                                      reinterpret_cast<void**>(&dxgiDev));
    if (FAILED(hr) || !dxgiDev) {
        Log("DXGI: QueryInterface(IDXGIDevice) failed");
        return;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDev->GetParent(__uuidof(IDXGIAdapter),
                             reinterpret_cast<void**>(&adapter));
    dxgiDev->Release();
    if (FAILED(hr) || !adapter) {
        Log("DXGI: GetParent(IDXGIAdapter) failed");
        return;
    }

    IDXGIFactory* factory = nullptr;
    hr = adapter->GetParent(__uuidof(IDXGIFactory),
                             reinterpret_cast<void**>(&factory));
    adapter->Release();
    if (FAILED(hr) || !factory) {
        Log("DXGI: GetParent(IDXGIFactory) failed");
        return;
    }

    void** factoryVtable = *reinterpret_cast<void***>(factory);
    // IDXGIFactory::CreateSwapChain is vtable slot 10.
    MH_STATUS st = MH_CreateHook(factoryVtable[10],
                                  reinterpret_cast<void*>(HookCreateSwapChain),
                                  reinterpret_cast<void**>(&g_origCreateSwapChain));
    if (st == MH_OK) {
        MH_EnableHook(factoryVtable[10]);
        Log("MinHook: IDXGIFactory::CreateSwapChain hooked OK");
    } else {
        Logf("MinHook: CreateSwapChain hook failed (%d)", st);
    }

    factory->Release();
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

static bool g_shaderHooked = false;

static void HookShaderCreation(ID3D11Device* dev)
{
    if (g_shaderHooked || !dev) return;
    g_shaderHooked = true;

    // Hook DXGI factory for swap chain Present interception.
    HookDXGIFactory(dev);

    void** vtable = *reinterpret_cast<void***>(dev);

    // Hook CreatePixelShader (vtable slot 15).
    MH_STATUS st = MH_CreateHook(vtable[15],
                                  reinterpret_cast<void*>(HookCreatePixelShader),
                                  reinterpret_cast<void**>(&g_origCPS));
    if (st == MH_OK) {
        MH_EnableHook(vtable[15]);
        Log("MinHook: CreatePixelShader hooked OK");
    } else {
        Logf("MinHook: CreatePixelShader hook failed (%d)", st);
    }

    // Hook CreateVertexShader (vtable slot 12).
    st = MH_CreateHook(vtable[12],
                        reinterpret_cast<void*>(HookCreateVertexShader),
                        reinterpret_cast<void**>(&g_origCVS));
    if (st == MH_OK) {
        MH_EnableHook(vtable[12]);
        Log("MinHook: CreateVertexShader hooked OK");
    } else {
        Logf("MinHook: CreateVertexShader hook failed (%d)", st);
    }

    // Hook device context for draw-time VS swapping.
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (ctx) {
        void** ctxVtable = *reinterpret_cast<void***>(ctx);

        // VSSetShader (slot 11).
        st = MH_CreateHook(ctxVtable[11],
                            reinterpret_cast<void*>(HookVSSetShader),
                            reinterpret_cast<void**>(&g_origVSSetShader));
        if (st == MH_OK) {
            MH_EnableHook(ctxVtable[11]);
            Log("MinHook: VSSetShader hooked OK");
        } else {
            Logf("MinHook: VSSetShader hook failed (%d)", st);
        }

        // PSSetShader (slot 9).
        st = MH_CreateHook(ctxVtable[9],
                            reinterpret_cast<void*>(HookPSSetShader),
                            reinterpret_cast<void**>(&g_origPSSetShader));
        if (st == MH_OK) {
            MH_EnableHook(ctxVtable[9]);
            Log("MinHook: PSSetShader hooked OK");
        } else {
            Logf("MinHook: PSSetShader hook failed (%d)", st);
        }

        // DrawIndexed (slot 12).
        st = MH_CreateHook(ctxVtable[12],
                            reinterpret_cast<void*>(HookDrawIndexed),
                            reinterpret_cast<void**>(&g_origDrawIndexed));
        if (st == MH_OK) {
            MH_EnableHook(ctxVtable[12]);
            Log("MinHook: DrawIndexed hooked OK");
        } else {
            Logf("MinHook: DrawIndexed hook failed (%d)", st);
        }

        // Draw (slot 13).
        st = MH_CreateHook(ctxVtable[13],
                            reinterpret_cast<void*>(HookDraw),
                            reinterpret_cast<void**>(&g_origDraw));
        if (st == MH_OK) {
            MH_EnableHook(ctxVtable[13]);
            Log("MinHook: Draw hooked OK");
        } else {
            Logf("MinHook: Draw hook failed (%d)", st);
        }

        ctx->Release();
    } else {
        Log("MinHook: GetImmediateContext failed");
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
        HookShaderCreation(*ppDev);
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
        HookShaderCreation(*ppDev);

    // Hook Present on the swap chain for per-frame state reset.
    if (SUCCEEDED(hr) && ppSC && *ppSC && !g_origPresent) {
        void** scVtable = *reinterpret_cast<void***>(*ppSC);
        // IDXGISwapChain::Present is vtable slot 8.
        MH_STATUS st = MH_CreateHook(scVtable[8],
                                      reinterpret_cast<void*>(HookPresent),
                                      reinterpret_cast<void**>(&g_origPresent));
        if (st == MH_OK) {
            MH_EnableHook(scVtable[8]);
            Log("MinHook: Present hooked OK");
        } else {
            Logf("MinHook: Present hook failed (%d)", st);
        }
    }

    return hr;
}

static void InstallD3D11Hooks()
{
    // Load D3DCompile and D3DDisassemble from d3dcompiler_47.dll.
    HMODULE hCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (hCompiler) {
        g_pD3DCompile = reinterpret_cast<PFN_D3DCompile>(
            GetProcAddress(hCompiler, "D3DCompile"));
        g_pD3DDisassemble = reinterpret_cast<PFN_D3DDisassemble>(
            GetProcAddress(hCompiler, "D3DDisassemble"));
        if (g_pD3DCompile)
            Log("D3DCompile: loaded from d3dcompiler_47.dll");
        else
            Log("D3DCompile: GetProcAddress failed");
        if (g_pD3DDisassemble)
            Log("D3DDisassemble: loaded from d3dcompiler_47.dll");
    } else {
        Log("D3DCompile: d3dcompiler_47.dll not found");
    }

    // Store game root for diagnostic dumps.
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        g_gameRoot = std::string(exePath);
        g_gameRoot = g_gameRoot.substr(0, g_gameRoot.rfind('\\') + 1);
    }

    // Compile all HUD replacement shaders.
    CompileHudShaders();

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
                g_vsShift  = (float)(1.0 - kStd / aspect);
                Logf("HUD shader: aspect=%.4f PS_shift=%.6f VS_shift=%.6f",
                     aspect, g_hudShift, g_vsShift);
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
