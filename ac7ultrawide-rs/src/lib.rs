// ac7ultrawide.asi – Rust implementation
// Runtime memory patcher for ACE COMBAT 7 ultrawide support.
// Loaded via Ultimate ASI Loader (version.dll in game root).
//
// Identical behaviour to the C++ version:
//   1. Black bar removal  – patches one byte in-memory
//   2. FOV fix            – patches 3 bytes in-memory (aspect-ratio-aware)
//   3. HUD shift          – rewrites the constant in Mods/hudtextfix.ini
//
// Requires the ORIGINAL (unpatched) Ace7Game.exe for patches 1 & 2.
// If magic.py was previously run, restore Ace7Game.exe from a .bak backup.

#![allow(non_snake_case)]

use std::ffi::c_void;
use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::os::windows::ffi::OsStringExt;
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};

// ---------------------------------------------------------------------------
// Raw Windows API declarations – no external crates required.
// ---------------------------------------------------------------------------

type HMODULE = isize;
type HANDLE  = isize;
type BOOL    = i32;

const DLL_PROCESS_ATTACH:     u32 = 1;
const PAGE_EXECUTE_READWRITE: u32 = 0x40;
const SM_CXSCREEN:            i32 = 0;
const SM_CYSCREEN:            i32 = 1;

#[link(name = "kernel32")]
extern "system" {
    fn GetModuleHandleW(lpModuleName: *const u16) -> HMODULE;
    fn GetModuleFileNameW(hModule: HMODULE, lpFilename: *mut u16, nSize: u32) -> u32;
    fn VirtualProtect(
        lpAddress:      *const c_void,
        dwSize:         usize,
        flNewProtect:   u32,
        lpflOldProtect: *mut u32,
    ) -> BOOL;
    fn FlushInstructionCache(
        hProcess:      HANDLE,
        lpBaseAddress: *const c_void,
        dwSize:        usize,
    ) -> BOOL;
    fn GetCurrentProcess() -> HANDLE;
    fn CreateThread(
        lpThreadAttributes: *const c_void,
        dwStackSize:        usize,
        lpStartAddress:     unsafe extern "system" fn(*mut c_void) -> u32,
        lpParameter:        *mut c_void,
        dwCreationFlags:    u32,
        lpThreadId:         *mut u32,
    ) -> HANDLE;
    fn CloseHandle(hObject: HANDLE) -> BOOL;
    fn Sleep(dwMilliseconds: u32);
    fn DisableThreadLibraryCalls(hLibModule: HMODULE) -> BOOL;
}

#[link(name = "user32")]
extern "system" {
    fn GetSystemMetrics(nIndex: i32) -> i32;
    fn SetProcessDPIAware() -> BOOL;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static LOG: OnceLock<Mutex<File>> = OnceLock::new();

fn log(msg: &str) {
    if let Some(m) = LOG.get() {
        if let Ok(mut f) = m.lock() {
            let _ = writeln!(f, "{}", msg);
        }
    }
}

fn init_log() {
    let path = game_root().join("ac7ultrawide.log");
    if let Ok(f) = OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .open(path)
    {
        let _ = LOG.set(Mutex::new(f));
    }
}

fn flush_log() {
    if let Some(m) = LOG.get() {
        if let Ok(mut f) = m.lock() {
            let _ = f.flush();
        }
    }
}

// ---------------------------------------------------------------------------
// Utility: directory containing Ace7Game.exe
// ---------------------------------------------------------------------------

fn game_root() -> PathBuf {
    let mut buf = vec![0u16; 1024];
    let len = unsafe {
        GetModuleFileNameW(0, buf.as_mut_ptr(), buf.len() as u32)
    } as usize;
    let path = PathBuf::from(std::ffi::OsString::from_wide(&buf[..len]));
    path.parent().map(|p| p.to_path_buf()).unwrap_or(path)
}

// ---------------------------------------------------------------------------
// Memory utilities
// ---------------------------------------------------------------------------

/// Scan `[base, base+size)` for the first exact match of `pat`.
unsafe fn find_pattern(base: *const u8, size: usize, pat: &[u8]) -> Option<*mut u8> {
    if pat.is_empty() || pat.len() > size {
        return None;
    }
    for i in 0..=(size - pat.len()) {
        if std::slice::from_raw_parts(base.add(i), pat.len()) == pat {
            return Some(base.add(i) as *mut u8);
        }
    }
    None
}

/// Write `data` to `addr`, temporarily making the page writable.
unsafe fn patch_bytes(addr: *mut u8, data: &[u8]) -> bool {
    let mut old: u32 = 0;
    if VirtualProtect(addr as *const c_void, data.len(), PAGE_EXECUTE_READWRITE, &mut old) == 0 {
        return false;
    }
    std::ptr::copy_nonoverlapping(data.as_ptr(), addr, data.len());
    VirtualProtect(addr as *const c_void, data.len(), old, &mut old);
    FlushInstructionCache(GetCurrentProcess(), addr as *const c_void, data.len());
    true
}

/// Locate the 3-byte FOV integer that sits immediately before the unique
/// context suffix `3C D8 F5`.
///
/// Tries the exact original pattern first (unpatched exe: `35 FA 0E 3C D8 F5`).
/// Falls back to a wildcard scan that accepts any 3 bytes before `3C D8 F5`
/// whose little-endian value is in the plausible FOV integer range, to handle
/// an exe that was already patched on disk by magic.py.
unsafe fn find_fov_location(base: *const u8, img_size: usize) -> Option<*mut u8> {
    // Exact match (original exe, 90° / 16:9 default).
    let orig: &[u8] = &[0x35, 0xFA, 0x0E, 0x3C, 0xD8, 0xF5];
    if let Some(p) = find_pattern(base, img_size, orig) {
        return Some(p);
    }

    // Wildcard: find `3C D8 F5` and validate the preceding 3 bytes are a
    // plausible 3-byte FOV int (covers 4:3 through 32:9 monitors).
    let ctx: &[u8] = &[0x3C, 0xD8, 0xF5];
    let limit = img_size.saturating_sub(3);
    for i in 3..=limit {
        if std::slice::from_raw_parts(base.add(i), 3) != ctx {
            continue;
        }
        let candidate = (*base.add(i - 3) as u32)
            | ((*base.add(i - 2) as u32) << 8)
            | ((*base.add(i - 1) as u32) << 16);
        if (0x05_0000..=0x7F_FFFF).contains(&candidate) {
            return Some(base.add(i - 3) as *mut u8);
        }
    }
    None
}

// ---------------------------------------------------------------------------
// HUD ini patch
// ---------------------------------------------------------------------------

/// Replace the shift constant in the ShaderRegex replace line of hudtextfix.ini.
///
/// Targets the literal text (backslash-n is two characters in the file):
///   ${0}\nadd ${register}, ${register}, l(VALUE)
/// and substitutes VALUE with `shift` formatted to 4 decimal places.
/// Safe to call on every launch – idempotent.
fn update_hud_shift(ini_path: &PathBuf, shift: f64) -> bool {
    let mut content = String::new();
    if File::open(ini_path)
        .and_then(|mut f| f.read_to_string(&mut content))
        .is_err()
    {
        return false;
    }

    // This marker is on the live replace line, not the commented copy above it.
    let marker = r"\nadd ${register}, ${register}, l(";
    let marker_pos = match content.find(marker) {
        Some(p) => p,
        None    => return false,
    };

    let value_start = marker_pos + marker.len();
    let value_end   = match content[value_start..].find(')') {
        Some(p) => value_start + p,
        None    => return false,
    };

    content.replace_range(value_start..value_end, &format!("{:.4}", shift));

    OpenOptions::new()
        .write(true)
        .truncate(true)
        .open(ini_path)
        .and_then(|mut f| f.write_all(content.as_bytes()))
        .is_ok()
}

// ---------------------------------------------------------------------------
// Main patch logic
// ---------------------------------------------------------------------------

unsafe fn apply_patches() {
    // -- Resolution ----------------------------------------------------------
    SetProcessDPIAware();
    let w = GetSystemMetrics(SM_CXSCREEN);
    let h = GetSystemMetrics(SM_CYSCREEN);
    let aspect = w as f64 / h as f64;
    const STD_ASPECT: f64 = 16.0 / 9.0;

    log(&format!("Resolution: {}x{}  aspect={:.6}", w, h, aspect));

    // -- FOV (same formula as magic.py) --------------------------------------
    // hfov = 2 * atan(aspect * (9/16))
    // This preserves the vertical FOV of a standard 16:9 screen at 90° H-FOV.
    let fov_rad = 2.0 * f64::atan(aspect * (9.0 / 16.0));
    let fov_deg = fov_rad.to_degrees();

    // Linear encoding found experimentally: int = round(129591 * deg - 10681633)
    // stored as a 3-byte little-endian integer in the exe.
    let fov_int = (129_591.0_f64 * fov_deg - 10_681_633.0).round() as i32;

    log(&format!("FOV: {:.2} deg  int=0x{:06X} ({})", fov_deg, fov_int as u32, fov_int));

    // -- HUD shift -----------------------------------------------------------
    // The game positions HUD elements using a 1080p-normalised coordinate space.
    // standardised_width = W * (1080 / H)
    // hud_shift = -(standardised_width - 1920) / 3840
    let std_w     = w as f64 * (1080.0 / h as f64);
    let hud_shift = -((std_w - 1920.0) / 3840.0);

    log(&format!("HUD shift: {:.4}", hud_shift));

    // -- Locate game module --------------------------------------------------
    let h_exe    = GetModuleHandleW(std::ptr::null());
    let base     = h_exe as *const u8;

    // Parse SizeOfImage from the PE optional header.
    // e_lfanew (IMAGE_DOS_HEADER + 0x3C) -> IMAGE_NT_HEADERS64
    // SizeOfImage: NTHeaders + 4 (sig) + 20 (FileHeader) + 56 (into OptionalHeader)
    let e_lfanew = *(base.add(0x3C) as *const u32) as usize;
    let img_size = *(base.add(e_lfanew + 4 + 20 + 56) as *const u32) as usize;

    log(&format!("Module base: {:p}  image size: 0x{:X}", base, img_size));

    // ========================================================================
    // Patch 1: Black bar / letterbox removal
    //
    //   48 81 EC D0 00 00 00   sub  rsp, 0D0h
    //   F6 41 2C 01            test byte [rcx+2Ch], 1   <- zero the immediate
    //
    // Changing the immediate 01 -> 00 makes the test always produce zero,
    // so the letterbox branch is never taken.
    // ========================================================================
    let bb_pat: &[u8] = &[
        0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00,
        0xF6, 0x41, 0x2C, 0x01,
    ];
    let mut bb_found = false;
    for attempt in 0..10 {
        if attempt > 0 {
            Sleep(200);
            log(&format!("Black bar patch: retry #{}", attempt));
        }
        match find_pattern(base, img_size, bb_pat) {
            Some(addr) => {
                if patch_bytes(addr.add(10), &[0x00]) {
                    log("Black bar patch: OK");
                } else {
                    log("Black bar patch: VirtualProtect failed");
                }
                bb_found = true;
                break;
            }
            None => continue,
        }
    }
    if !bb_found {
        log("Black bar patch: pattern not found after retries (may already be patched or game version changed)");
    }

    // ========================================================================
    // Patch 2: Field of view
    //
    // The 3-byte little-endian FOV int sits just before context bytes 3C D8 F5.
    // Default (16:9, 90°) = 35 FA 0E (int 981557 = 0x0EFA35).
    // Only applied when the screen is wider than 16:9.
    // ========================================================================
    if aspect > STD_ASPECT + 0.01 {
        if fov_int <= 0 || fov_int > 0xFF_FFFF {
            log(&format!("FOV patch: int out of range ({}) - skipped", fov_int));
        } else {
            match find_fov_location(base, img_size) {
                Some(addr) => {
                    let bytes = [
                        ( fov_int        & 0xFF) as u8,
                        ((fov_int >>  8) & 0xFF) as u8,
                        ((fov_int >> 16) & 0xFF) as u8,
                    ];
                    if patch_bytes(addr, &bytes) {
                        log(&format!(
                            "FOV patch: OK  bytes={:02X} {:02X} {:02X}",
                            bytes[0], bytes[1], bytes[2]
                        ));
                    } else {
                        log("FOV patch: VirtualProtect failed");
                    }
                }
                None => log("FOV patch: pattern not found"),
            }
        }
    } else {
        log("FOV patch: skipped (16:9 or narrower)");
    }

    // ========================================================================
    // Patch 3: Mods/hudtextfix.ini – update the HUD shift constant
    // ========================================================================
    let hud_ini = game_root().join("Mods").join("hudtextfix.ini");
    if hud_ini.exists() {
        if update_hud_shift(&hud_ini, hud_shift) {
            log(&format!("HUD ini patch: OK  (shift={:.4})", hud_shift));
        } else {
            log("HUD ini patch: marker not found in file");
        }
    } else {
        log("HUD ini patch: Mods/hudtextfix.ini not found - skipped");
    }

    log("Done.");
}

// ---------------------------------------------------------------------------
// Thread + DLL entry point
// ---------------------------------------------------------------------------

unsafe extern "system" fn patch_thread(_: *mut c_void) -> u32 {
    Sleep(500); // let the game loader + Steam DRM stub finish decrypting .text
    apply_patches();
    flush_log();
    0
}

#[no_mangle]
pub unsafe extern "system" fn DllMain(
    hmodule:   HMODULE,
    call_reason: u32,
    _reserved: *mut c_void,
) -> BOOL {
    if call_reason == DLL_PROCESS_ATTACH {
        DisableThreadLibraryCalls(hmodule);
        init_log();
        log("ac7ultrawide ASI loaded");
        let handle = CreateThread(
            std::ptr::null(),
            0,
            patch_thread,
            std::ptr::null_mut(),
            0,
            std::ptr::null_mut(),
        );
        if handle != 0 {
            CloseHandle(handle);
        }
    }
    1 // TRUE
}
