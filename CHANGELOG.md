# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [Unreleased]

- Ongoing work on reducing the post-restore aim input delay

---

## [0.2.0-beta] - 2026-04-04

### Fixed
- **Mid-swing RB release**: releasing RB during a knife attack no longer instantly
  swaps back to the pistol. The attack animation now plays to completion before
  the weapon is restored (swing drain — waits for dispatch to return to idle, or
  90-frame cap).
- **LB loop after restore**: pressing the aim button immediately after releasing RB
  no longer causes a pistol aim + fire animation loop. Weapon-action inputs are
  suppressed for a short settling window after the pistol model is reloaded.
- **Flicker gap on drain restore**: the single-frame gap between swing drain
  completing and the restore one-shot firing is now covered by the present
  suppression budget, eliminating the flicker that appeared at the end of a
  drained swing.

### Known remaining issues
- A brief aim input delay (~130 ms) is noticeable after releasing RB — inputs are
  blocked while the weapon state settles.
- Character can freeze if a zombie grabs the player's leg while RB is held.
- See [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for the full list.

---

## [0.1.0] - 2026-03-30

### Added
- Standalone public project layout extracted from the larger private workspace
- Visual Studio solution and project files for the current quick-knife branch
- Dedicated `build.bat` for building `quick_knife_codex.dll`
- Public-facing `README.md` and `KNOWN_ISSUES.md`

### Implemented
- Quick knife activation on XInput **RB**
- Hidden-slot knife handling
- Weapon restore path back to the previously equipped firearm
- Render-side suppression logic intended to reduce restore flicker

### Notes
- This snapshot reflects the current working Codex branch as of March 30, 2026
- The project is still experimental and has unresolved animation issues
