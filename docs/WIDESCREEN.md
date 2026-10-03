# Widescreen support — what works, what breaks, and what to change

Measured 2026-08-05 on commit `7dd094e` + `MC2_SCREEN`. Every claim below is
backed by a render at a real resolution, not by reading the matrix code.

Run any resolution yourself:

```
MC2_SCREEN=1920x1080
```

`MC2_SCREEN=WxH` overrides whatever mode the game asks for. It remains a silent,
opt-in diagnostic override. As of 2026-08-09, the in-game Graphics list also
offers persistent 1920×1080 choices and `options.cfg` stores exact
`ScreenWidth`/`ScreenHeight`; index-only retail configs still migrate unchanged.
The modern 32-bit catalogue now spans common 720p through 5K, 16:10, and
ultrawide modes while retaining all ten original resolution/depth entries.

**That catalogue is filtered by the display, and only became so on 2026-08-09.**
`gos_Info_ValidMode` returned an unconditional 1 — correct while the table held
ten 4:3/5:4 modes any adapter can show, and wrong the moment it reached
5120×2880, because it meant a 1080p player was offered 5K and `applyPrefs` would
hand that size straight to `gos_SetScreenMode` with nothing to fall back to.
Contemporary notes describing the list as "subject to platform validation" were
describing an intention, not the code. It is now bounded by the primary display
with a floor at 640×480, and `applyPrefs` falls back to 800×600 when a saved
pair is rejected — which matters because `options.cfg` travels between machines.
A host that cannot report a display size still answers 1, so headless runs are
unaffected.

---

## The headline

**The 3D renderer is already widescreen-correct. Nothing is stretched, skewed or
distorted at 16:9.** The camera derives its clip planes from
`Environment.screenHeight / screenWidth` in both projection modes
(`Camera::setOrthogonal`, `MCLib/Camera.cpp:1848` and `:1892`), so the aspect is
handled honestly at any size. Artifact:
`docs/artifacts/m6-widescreen-1920x1080-16by9.png`.

**But the projection is horizontal-locked, so widescreen shows you _less_ of the
battlefield, not more.** That is the real problem, and it is not obvious from a
single screenshot — it needs a controlled comparison.

### The measurement that shows it

Same mission, same frame 9,000, same **1920 px width**, aspect the only variable:

| render | file | horizontal world visible | vertical world visible |
|---|---|---|---|
| 1920×1440 (4:3) | `m6-widescreen-1920x1440-4by3.png` | baseline | baseline |
| 1920×1080 (16:9) | `m6-widescreen-1920x1080-16by9.png` | **identical** | **75% — a quarter is gone** |

The prison complex occupies the same horizontal span in both. The 4:3 render
shows fences, guard towers and tank tracks in the lower third that the 16:9
render simply does not contain.

That is what the matrix says it should do. In the orthogonal path:

```cpp
left_clip = invCamScale * 300.0;                       // world half-width: CONSTANT
top_clip  = left_clip * (screenHeight / screenWidth);  // world half-height: shrinks
```

World half-width is fixed, and half-height is scaled by `H/W`. Going 4:3 → 16:9
takes `H/W` from 0.75 to 0.5625, so vertical extent drops to 75%. A widescreen
player is at a **disadvantage** to a 4:3 player. For an isometric RTS this is
backwards: the convention you want is "Hor+" — keep the vertical extent, gain
world at the sides.

---

## What actually breaks, in priority order

| # | Layer | Behaviour at 16:9 | Where |
|---|---|---|---|
| 1 | 3D field of view | Lost 25% of vertical world — **FIXED** | `MCLib/Camera.cpp:1848`, `:1892` |
| 2 | 2D UI (menus, splash, mech bay) | Drawn at authored 800×600 pixel coordinates in the **top-left corner**; the rest of the screen is empty | `MainMenu.cpp:210`, `logistics.cpp:133`, `MissionBegin.cpp:192` force an 800×600 mode |
| 3 | In-mission HUD | Falls back to `buttonlayout1024.fit` for any unrecognised width | `controlGui.cpp:2657` |
| 4 | Unit scale | Scaled with **width**, so units grew as the window widened while the world did not — **FIXED** | `gamecam.cpp:129` |

Artifact for #2: `docs/artifacts/m6-widescreen-menu-1920x1080.png` — the splash
sits in a 800×600 box in the corner with 58% of the screen unused.

`ControlGui::swapResolutions` keys on **width alone** and knows five files
(`buttonlayout640/800/1024/1280/1600.fit`), each ~3,500 lines of absolute pixel
coordinates. 1920 matches nothing, so it silently loads the 1024 layout.

---

## Status

**Items 1 through 4 below are DONE and measured.** The projection is
vertical-locked, unit scale follows the locked axis, the authored 800x600
logistics canvas uniformly fits and centres in the real backbuffer, and mouse
coordinates apply the inverse transform. Item 5 remains open.

The 2D proof used a 1920x1080 WebGPU surface: the canvas scaled by 1.8 and
pillarboxed by 240 pixels on each side. A genuine hardware-level click at the
transformed OPTIONS coordinate opened the options screen. That same screen
showed `MechCommander 2 Display Adapter`, proving the pointer-valued machine
query survives the 64-bit ABI. Automated capture passed; owner playtest remains
the final acceptance gate before progress credit.

The first owner launch exposed two follow-up defects, repaired on 2026-08-09.
The retail loading wrapper used `Environment.screenWidth == 800` as a hidden
entrance/exit phase flag; preserving the 1920-wide backbuffer made it render an
uninitialized exit screen and crash in `aAnimation::getXDelta`. It now carries
explicit phase state and uses 800x600 loading art while the virtual canvas is
active. The Win32 peer also fits a requested client into the monitor work area
and centres it rather than creating a 1920x1080 client plus borders offscreen.

The second owner launch exposed the remaining canvas lifetime bugs. Tactical
terrain was fixed by clearing the logistics canvas before mission play, but the
clear was too early for the final loading exit frames. It now happens at the
actual `log_DONE` handoff. In-mission options are also scoped to the fixed
800x600 canvas while the full-screen overlay is active, and modern saved modes
no longer assert in `LoadScreenWrapper::changeRes()` when accepting settings.
Owner re-test of pause -> Options -> accept and the last loading frames remains
pending.

Measured clip planes, `MC2_CAMERA_CENSUS=1`:

```text
1600x1200  (4:3)   left=145.5881  right=-145.5881  top=-109.1911  bottom=109.1911
1920x1440  (4:3)   left=145.5881  right=-145.5881  top=-109.1911  bottom=109.1911
1920x1080 (16:9)   left=194.1175  right=-194.1175  top=-109.1911  bottom=109.1911
2560x1080 (21:9)   left=258.8233  right=-258.8233  top=-109.1911  bottom=109.1911
1280x1024  (5:4)   left=145.5881  right=-145.5881  top=-116.4705  bottom=116.4705
```

Vertical extent is now **identical** across every aspect at or wider than 4:3,
and horizontal widens by exactly 1.3333x at 16:9 and 1.7778x at 21:9. 5:4 keeps
its original, taller planes — anything 4:3-or-narrower takes the untouched code
path, so all five stock modes are bit-identical and the strict fixed-60 hashes
are unchanged (verified 6/6 at 200 and 3/3 at 2000).

Artifacts `m6-widescreen-fixed-1920x1080.png` and `-1920x1440.png` are the same
frame at both aspects: the same two guard towers at the same vertical position,
with trees visible at both edges of the 16:9 frame that the 4:3 frame does not
contain.

**Two traps this change actually hit, recorded because neither was hypothetical:**

1. Writing the vertical clip plane out by hand instead of deriving it from
   `left_clip` **flipped the sign of the vertical axis**. The screenshot still
   looked like a plausible scene; only printing the raw clip planes caught it.
2. Frame renders are **not deterministic run to run** — identical config gives
   different pixels — so comparing two .ppm files proves nothing about a code
   change. The strict fixed-60 stdout hash is the deterministic instrument.

---

## Recommended changes

### 1. Vertical-locked projection — DONE (`Camera.cpp`)

The vertical extent is anchored to the 4:3 view and horizontal is derived from
it, so a wider screen *adds* world at the sides instead of cropping top and
bottom. Both projection paths changed, plus the sphere-cull bounds in
`calculateProjectionConstants` — the culling heuristic grows far slower than the
real horizontal FOV, so left alone it culls objects that are still on screen.

`MC2_WidescreenFactor()` returns **exactly `1.0f`** unless `W*3 > H*4`. Every
stock mode is 4:3 or 5:4, so they all take the original expressions verbatim and
their clip planes are bit-identical by construction rather than by luck. That is
why the strict fixed-60 hashes could not move — and they did not.

The vertical planes are derived from `left_clip` in the same shape as the
original line, with `0.75f` pinned in place of `height2width`. **Writing that
expression out by hand instead flipped the sign of the vertical axis** and the
resulting screenshot still looked like a plausible scene. See the traps above.

**Design note worth raising before doing it:** this makes widescreen players see
more map than 4:3 players. For a single-player fan port that is the point. If
multiplayer parity ever matters, it becomes a fairness question.

### 2. Unit scale must follow height, not width — DONE (gamecam.cpp)

`gamecam.cpp:129` is `globalScaleFactor *= viewMulX / 640.0`. Once the
projection is vertical-locked, scale has to key off the locked axis:

```cpp
globalScaleFactor *= viewMulY / 480.0;
```

Change these two together or units will be the wrong size at every aspect but
4:3. (`Camera.cpp:1631` and `:1736` use `viewMulX / Environment.screenWidth`,
which is a viewport-to-screen ratio of 1.0 in normal play — leave those alone.)

### 3. 2D UI: scale a virtual canvas, don't re-author the layout

Two options, and the cheap one is much better value:

**(a) Virtual canvas — recommended.** Let the UI keep drawing at its authored
800×600 (or 1024×768) coordinates, then scale that canvas uniformly to fit the
real backbuffer and centre it. The UI reaches the renderer as pre-transformed
`gos_VERTEX` screen coordinates, so this is one scale-and-offset applied at the
GameOS seam — no art changes, no `.fit` edits, correct at every resolution
including ultrawide. Cost: pillarboxing on the menus, which is what every
widescreen patch of a fixed-layout game does.

**(b) Anchor-based re-layout.** Load `buttonlayout1024.fit` and offset
right-anchored elements by `W-1024`, bottom-anchored by `H-768`. This looks more
native — HUD pushed out to the real corners — but it needs every one of ~3,500
entries classified by anchor, and MC2's HUD art is a continuous bitmap frame
rather than separable widgets, so stretched regions will show seams. Worth it
only for the in-mission HUD, and only if someone will author new art.

Do (a) first. It makes every screen usable at any resolution; (b) is polish.

### 4. Mouse mapping is what breaks first

Input already scales viewport→game coordinates (M4-5). The moment a virtual
canvas is added, the **inverse** transform must be applied to mouse positions or
every click lands somewhere other than where the cursor is. Whoever implements
(3) should change the input mapping in the same commit and verify with a real
click, not by reading the code.

### 5. Make the resolution real, not an env var — implemented 2026-08-09

`MC2_SCREEN` remains a diagnostic. `CPrefs` now persists exact width/height,
migrates the retail resolution index, and applies the saved pair directly. The
existing drop-list retains all ten retail entries and adds common modern modes
at 32-bit from 1152×864 through 5120×2880. On 2026-08-09, the normal launcher
selected and saved `1920 x 1080 x 32`, relaunched without `MC2_SCREEN`, and
entered tactical play with the accepted dock intact at the saved mode.

---

## Order of work

1. Projection + unit scale (1 and 2) — this is "the game plays in widescreen".
2. Virtual canvas + mouse mapping (3a and 4) — this is "the UI is usable".
3. Resolution in prefs (5) — this is "a player can choose it".
4. HUD re-layout (3b) — optional polish, needs art.

Steps 1 and 2 are small and self-contained. Step 2 is the one with real surface
area, and its risk is entirely in the input mapping.
