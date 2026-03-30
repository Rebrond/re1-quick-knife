# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [Unreleased]

- Ongoing work on animation timing and remaining restore-side visual glitches

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
