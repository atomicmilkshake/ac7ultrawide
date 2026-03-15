# ac7ultrawide

Runtime ultrawide fix for **ACE COMBAT 7: SKIES UNKNOWN** (PC / Steam).

Loaded automatically at game startup via [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader). No permanent changes are made to the game executable.

## What it does

| Patch | Description |
|-------|-------------|
| Black bar removal | Disables the letterbox that the game forces on ultrawide resolutions |
| FOV fix | Widens the horizontal field of view to match your aspect ratio, preserving the original vertical FOV |
| HUD shift | Updates `Mods/hudtextfix.ini` with the correct shift constant so HUD elements stay centred |

Patches 1 and 2 work by scanning the game's in-memory image and writing a small number of bytes at runtime. The game executable is never modified on disk.

## Requirements

- ACE COMBAT 7: SKIES UNKNOWN (Steam, original/unpatched `Ace7Game.exe`)
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) — place `version.dll` in the game root
- For the HUD patch: [hudtextfix (Nexus Mods)](https://www.nexusmods.com/acecombat7skiesunknown/mods/408) installed under `Mods/hudtextfix.ini`

## Installation

1. Build the project (see below), or download a pre-built release.
2. Copy `ac7ultrawide.asi` to your game root (next to `Ace7Game.exe`).
3. Ensure `version.dll` (Ultimate ASI Loader) is also in the game root.
4. Launch the game normally. A log file `ac7ultrawide.log` will be written to the game root on each run.

## Building

### Rust (recommended)

```bat
cd ac7ultrawide-rs
build.bat
```

Requires the MSVC toolchain and `cargo`. The `.cargo/config.toml` sets the target to `x86_64-pc-windows-msvc`. The output DLL is at `target/x86_64-pc-windows-msvc/release/ac7ultrawide.dll` — rename it to `ac7ultrawide.asi`.

### C++

```bat
cmake -B build -A x64
cmake --build build --config Release
```

Requires CMake 3.20+ and MSVC. The output is `build/Release/ac7ultrawide.asi`.

Optionally, pass `-DGAME_DIR="..."` to have CMake copy the `.asi` directly into the game folder after each build:

```bat
cmake -B build -A x64 -DGAME_DIR="C:/SteamLibrary/steamapps/common/ACE COMBAT 7"
```

## Notes

- The game uses Steam DRM which encrypts the `.text` section on disk. The ASI waits 500 ms after load and retries the black bar pattern scan up to 10 times (at 200 ms intervals) to give the DRM stub time to finish decrypting memory.
- If you previously used `magic.py` to patch the exe on disk, restore `Ace7Game.exe` from its `.bak` backup before using this ASI.
- The FOV fix is only applied when your aspect ratio is wider than 16:9.
