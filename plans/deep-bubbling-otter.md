# Plan: Fix Black Bar Patch Timing for Steam DRM

## Context

The ac7ultrawide ASI plugin is deployed to the ACE COMBAT 7 game folder and handles 3 patches:
1. Black bar / letterbox removal
2. FOV fix for ultrawide
3. HUD shift in `Mods/hudtextfix.ini`

Patches 2 and 3 work. Patch 1 (black bars) appears broken — the byte pattern `48 81 EC D0 00 00 00 F6 41 2C 01` was not found when scanning the exe on disk.

**Root cause:** The exe has Valve/Steam DRM (`VLV` signature at offset 0x40, `.bind` section present). The `.text` section (code) is partially **encrypted on disk**. The FOV pattern lives in `.rdata` (data, unencrypted) so it's visible on disk. The black bar pattern is machine code in `.text` (encrypted on disk, decrypted at runtime by the Steam DRM stub).

**Evidence:** The Steam Deck fork (Flumeded/ac7-ultrawide-steamdeck) uses **Steamless** to decrypt the exe on disk first, then runs the same `magic.py` with the same patterns — and it works on this exact Nov 2022 exe (confirmed Dec 2025). The patterns haven't changed; they're just encrypted at rest.

**The risk:** Our ASI's patch thread does `Sleep(100)` then scans memory. If the Steam DRM stub hasn't finished decrypting `.text` by then, the scan fails. 100ms is probably enough, but there's no guarantee.

## Changes

**File:** `V:\#bullshit\ac7ultrawide-rs\src\lib.rs`

### 1. Add retry logic to the black bar scan

Replace the single-shot scan with a retry loop. If the pattern isn't found on first try (DRM may still be decrypting), wait and retry a few times before giving up.

In the `patch_thread` function (~line 346), change the approach:
- Initial delay: keep `Sleep(100)`
- After `apply_patches()`, if black bar patch logged "not found", retry with increasing delays
- OR: refactor `apply_patches()` to return whether each patch succeeded, then retry only the black bar scan

**Simpler approach** — modify `patch_thread` to retry the black bar scan specifically:

```
unsafe extern "system" fn patch_thread(_: *mut c_void) -> u32 {
    // Wait for the game loader + Steam DRM to finish decrypting .text
    Sleep(500);   // increased from 100ms — gives DRM stub more time
    apply_patches();
    flush_log();
    0
}
```

But a more robust approach is a retry loop for the black bar patch only:

```rust
// In apply_patches(), change the black bar section to:
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
```

### 2. Increase initial delay

Change `Sleep(100)` to `Sleep(500)` in `patch_thread`. This gives the Steam DRM stub more time to decrypt `.text` before we scan. The game takes seconds to fully load anyway, so 500ms is imperceptible.

### 3. Rebuild and redeploy

- Run `build.bat` from `V:\#bullshit\ac7ultrawide-rs\`
- Copy the built DLL as `ac7ultrawide.asi` to the game folder

## Verification

1. Build: `cd V:\#bullshit\ac7ultrawide-rs && cmd //c build.bat`
2. Deploy: copy `target/x86_64-pc-windows-msvc/release/ac7ultrawide.dll` to `F:/SteamLibrary/steamapps/common/ACE COMBAT 7/ac7ultrawide.asi`
3. Launch ACE COMBAT 7
4. Check `F:/SteamLibrary/steamapps/common/ACE COMBAT 7/ac7ultrawide.log` for:
   - "Black bar patch: OK" (confirms DRM decrypted and pattern found)
   - "FOV patch: OK" (already working)
   - "HUD ini patch: OK" (already working)
5. Visually confirm: no black bars on ultrawide, correct FOV, centered HUD
