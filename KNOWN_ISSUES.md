# Known Issues

## Active

### Knife prepare animation can be skipped
**What happens:** Sometimes pressing **RB** goes directly into knife aim instead of showing the intended prepare / raise animation first.

**Current understanding:** The quick-knife path can still enter a state where the gameplay side is ready before the animation side has visibly stepped through the expected transition.

**Status:** Under investigation.

---

### Releasing RB can briefly snap to pistol-ready, then lower immediately
**What happens:** After releasing **RB**, the character can briefly show a pistol-ready or pistol-aim pose and then immediately lower the weapon.

**Current understanding:** The restore path is better than earlier builds, but the game can still expose a transitional firearm state before the final lowered state settles.

**Status:** Under investigation.

---

### Flicker is reduced, but not yet fully verified as fixed
**What happens:** Short tests look better than earlier builds, but longer testing is still needed before the visual glitch can be called solved.

**Current understanding:** Restore-side present suppression helped, but the behavior still needs broader gameplay coverage.

**Status:** Needs more playtesting.

---

## Resolved

No resolved issues have been recorded in this standalone public changelog yet. Historical experimentation happened in the larger private workspace before this repo was split out.
