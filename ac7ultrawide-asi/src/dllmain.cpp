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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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

        CloseHandle(CreateThread(nullptr, 0, PatchThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
