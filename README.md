# RE1 Quick Knife

A DLL gameplay mod for **Resident Evil 1 PC (Classic REBirth)** that adds a dedicated quick-knife action on **XInput Right Bumper** while preserving the currently equipped firearm.

This repository is a standalone snapshot of the current working quick-knife branch extracted from a larger private workspace on **March 30, 2026**. It is intended to be GitHub-ready, reproducible, and easier to iterate on separately from the main `analog3d` project.

The public source file in this repo is intentionally named `dllmain_noflicker.cpp` because it comes from the active Codex working copy, not from the separate Claude-oriented `dllmain.cpp` file in the original workspace.

## Current status

This is a **work-in-progress gameplay build**, not a final release.

What currently works:
- Hold **RB** to switch into quick knife without manually opening the inventory
- Release **RB** to restore the previously equipped weapon
- Uses a hidden knife slot so the player can keep the real inventory layout intact
- Includes render-side suppression intended to reduce the visible flicker during weapon restore

Known remaining issues:
- Sometimes the knife prepare animation is skipped and the character enters knife aim immediately
- Sometimes releasing **RB** briefly shows a weapon-ready / weapon-lower snap
- Flicker has been reduced compared to earlier builds, but longer playtesting is still required

See [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for the live list.

## Controls

| Input | Action |
|-------|--------|
| XInput **RB** | Hold quick knife |
| XInput **Y**  | Inventory latch handling used by the mod during inventory preview flow |

The current code reads controller state through `XInputGetState`.

## Technical overview

The mod is a 32-bit DLL loaded by the Classic REBirth mod loader through `manifest.txt`.

At runtime it:
1. Hooks the game's pad write site at `0x483527`
2. Detects **RB** from XInput
3. Temporarily swaps the active weapon state to knife
4. Schedules weapon setup through the game's safer weapon-update paths instead of forcing all visual state from the input hook
5. Restores the previous firearm on button release
6. Uses DirectDraw present suppression to hide transitional restore frames

The code also contains extensive logging because this project was developed through reverse engineering, runtime tracing, and repeated Ghidra-assisted iteration.

## Requirements

- Resident Evil 1 PC with **Classic REBirth**
- XInput-compatible controller
- Visual Studio with Win32 C++ toolchain

The mod was developed against the GOG PC release used in the main workspace. Compatibility with other executables is not guaranteed.

## Build

This public snapshot is currently source-focused.

Local build scripts and Visual Studio project files are intentionally not part of the public repository yet, because this snapshot was published primarily to expose the current working quick-knife source and documented bug state.

## Installation

1. Build the project
2. Create a mod folder next to the game executable, for example `mod_quickknife_codex`
3. Copy `Release\quick_knife_codex.dll` and `manifest.txt` into that folder
4. Launch the game through Classic REBirth

## Repository layout

- `dllmain_noflicker.cpp` - current Codex quick-knife working snapshot
- `framework.h` - minimal Windows include shim
- `manifest.txt` - Classic REBirth loader manifest
- `CHANGELOG.md` - public project history
- `KNOWN_ISSUES.md` - active unresolved issues

## Credits

This mod was developed through collaboration between the project owner and AI coding assistants during reverse-engineering and gameplay iteration.

The memory research for Resident Evil 1 in the broader workspace relied in part on **[Gemini-Loboto3's RE1-Mod-SDK](https://github.com/Gemini-Loboto3/RE1-Mod-SDK)**.



