# Known Issues

## Active

### Brief aim input delay after releasing RB
**What happens:** After releasing RB, aim and weapon-action inputs are blocked for approximately 130 ms (8 frames at 60 fps) while the weapon state settles back to the pistol.

**Current understanding:** The block is intentional — it prevents a pistol aim + fire animation loop that occurs when the aim button is pressed during the restore window. The window is as short as practical but still noticeable.

**Status:** Under investigation — looking for a tighter condition that releases input sooner.

---

### Character freezes when grabbed by a zombie while holding RB
**What happens:** If a zombie grabs the player's leg while RB is held, the character becomes completely frozen.

**Current understanding:** The mod keeps injecting KEY_READY and re-asserting PL_ACTIVE_SLOT during the grab animation, which conflicts with the game's grab state machine.

**Status:** Not yet investigated.

---

### Knife prepare animation can occasionally be skipped
**What happens:** Sometimes pressing RB goes directly into knife aim instead of showing the intended prepare animation first.

**Current understanding:** The quick-knife path can still enter a state where the gameplay side is ready before the animation side has visibly stepped through the expected transition.

**Status:** Under investigation.

---

### Releasing RB can briefly snap to weapon-ready then lower
**What happens:** After releasing RB, the character can briefly show a weapon-ready or weapon-aim pose before settling.

**Current understanding:** The restore path is better than earlier builds, but the game can still expose a transitional firearm state before the final lowered state settles.

**Status:** Under investigation.

---

## Resolved

### Pistol aim + fire loop when pressing aim button immediately after RB release
**Fixed in:** 0.2.0-beta
Weapon-action inputs (KEY_READY, KEY_AIM) are now suppressed for a short settling window after the pistol model is reloaded.

---

### Flicker at end of mid-swing drain restore
**Fixed in:** 0.2.0-beta
The single-frame gap between swing drain completing and the restore one-shot firing is now covered by the present suppression budget.

---

### Instant weapon swap when releasing RB mid knife-attack swing
**Fixed in:** 0.2.0-beta
The attack animation now plays to completion before the weapon is restored.
