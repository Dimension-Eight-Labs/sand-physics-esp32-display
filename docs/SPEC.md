# Falling-Sand Phosphor Demo — Specification

Firmware for the **Waveshare ESP32-S3-LCD-1.47B**: a tilt-driven falling-sand
simulation rendered in the style of an old monochrome phosphor monitor.

This document is the source of truth for the design. It is kept in sync with
the code; deviations from the original brief are listed in §11.

---

## 1. Goals

- About one third of the screen is filled with glowing monochrome sand grains.
- Tilting the board makes sand flow, pile and slide; piles settle at a
  sand-like angle of repose (~30–35°), not flat like water.
- Shaking the board splashes the sand; it then resettles.
- Board lying flat: sand stays put with minimal drift.
- Mostly needs to look cool.

## 2. Hardware

All values live in `src/board.h`, each with a citation. Sources:

- **[WIKI]** <https://www.waveshare.com/wiki/ESP32-S3-LCD-1.47B> (pin table)
- **[IDF-DEMO]** `ESP32-S3-LCD-1.47B-Demo.zip` → `ESP-IDF/ESP32-S3-LCD-1.47B-Test`
- **[ARD-DEMO]** same zip → `Arduino/examples/LCD_Image`, `LVGL_Arduino`
- **[SCH]** `ESP32-S3-LCD-1.47B_schematic_diagram.pdf`

| Item | Value | Source |
|---|---|---|
| MCU | ESP32-S3R8 (8 MB octal PSRAM in-package), 16 MB flash | brief, [IDF-DEMO] sdkconfig.defaults (`SPIRAM_MODE_OCT`, `FLASHSIZE_16MB`) |
| LCD controller | ST7789 (Waveshare calls it ST7789T), 172×320, SPI mode 0 | [WIKI], [IDF-DEMO] `ST7789.h` |
| LCD SCLK / MOSI | GPIO40 / GPIO45 | [WIKI], [IDF-DEMO], [ARD-DEMO] |
| LCD CS / DC / RST | GPIO42 / GPIO41 / GPIO39 | [WIKI], [IDF-DEMO], [ARD-DEMO] |
| LCD backlight | GPIO46, active high, PWM | [WIKI], [IDF-DEMO] (LEDC) |
| LCD column offset | X +34, Y +0 (portrait) | [IDF-DEMO] `Offset_X/Offset_Y`, [ARD-DEMO] same; **confirmed on hardware** (all border edges visible) |
| LCD inversion | ON (`INVON` 0x21 in init) | [IDF-DEMO] and [ARD-DEMO] init sequences; **confirmed on hardware** |
| LCD MADCTL / colour order | 0x00 (RGB, no mirror) | [ARD-DEMO]; **confirmed on hardware** (test pattern) |
| LCD byte order | RGB565 little-endian (low byte first) | **confirmed on hardware** (see §11); matches RAMCTRL `00 E8` in both demos |
| SPI clock | 80 MHz | [ARD-DEMO] `SPIFreq 80000000` ([IDF-DEMO] uses 12 MHz); static test pattern clean on hardware, sustained streaming not yet seen |
| IMU | QMI8658, I2C SDA GPIO48, SCL GPIO47, 400 kHz | [IDF-DEMO] `I2C_Driver.h`, [ARD-DEMO], [SCH] `IMU_SDA/IMU_SCL` |
| IMU address | 0x6B (SA0 tied low), WHO_AM_I 0x05 | [IDF-DEMO] `QMI8658_L_SLAVE_ADDRESS`, [SCH] SA0 → GND; **confirmed on hardware** (0x05 at 0x6B) |
| IMU axes → screen | X = −chip X, Y = +chip Y, Z = chip Z (screen normal) | **hardware test** (§6); Y and Z confirmed, X sign flipped and to be re-checked |
| RGB LED | 1× WS2812B-0807 on GPIO38, GRB order | [SCH] `LED2 WS2812B-0807`, `RGB_IO`=IO38; [IDF-DEMO] `led_strip`, [ARD-DEMO] `neopixelWrite`; **colour order confirmed on hardware** (pure red shows red) |
| BOOT button | GPIO0, active low | [IDF-DEMO] `BOOT_KEY_PIN 0` |
| Console | Native USB (USB-Serial-JTAG), no UART bridge | [SCH] USB D+/D− on IO19/IO20; **confirmed on hardware** (banner and WHO_AM_I received) |

## 3. Toolchain

- PlatformIO, `framework = espidf`, base board `esp32-s3-devkitc-1`,
  flash overridden to 16 MB, octal PSRAM enabled via `sdkconfig.defaults`.
- ESP-IDF native drivers: `esp_lcd` (built-in ST7789 panel driver + Waveshare's
  vendor register sequence), SPI master with DMA, new I2C master driver
  (`driver/i2c_master.h`), LEDC for backlight, `espressif/led_strip`
  (RMT backend) for the WS2812B.
- C11.

## 4. Architecture

```
src/
  board.h        pins + panel constants, each with source citation
  config.h       every tunable: app mode, material structs, look table, grain size, look
  sim.[ch]       grid + grains + step function, both resting rules, water levelling (NO ESP-IDF headers)
  material.[ch]  material table lookup                   (NO ESP-IDF headers)
  palette.[ch]   phosphor palettes + RGB565 LUT  (NO ESP-IDF headers)
  render.[ch]    glow/persistence buffer, frame composition (NO ESP-IDF headers)
  display.[ch]   ST7789 init, async DMA push, backlight PWM
  imu.[ch]       QMI8658 driver, axis mapping, low-pass, shake detection (own task)
  button.[ch]    BOOT button debounce, short/long press events
  led.[ch]       WS2812B status pulse
  main.c         app: modes; sand = sim task (core 0) + render task (core 1)
tools/
  sim_golden.c   PC test: CLASSIC reproduces Classic physics v1 bit for bit
  sim_host.c     PC harness: repose angles, tilt/lean/flat behaviour, shake recovery (classic)
  sim_water.c    PC harness: water levelling, material switching, tremor / wake
  sim_hand.c     PC harness: pool / pile held in hand (tremor, sway, lean wobble): awake + cost
  sim_bench.c    PC benchmark: µs per step per material, six scenarios
```

`sim`, `material`, `palette` and `render` only use the C standard library and
`config.h`, so they compile on a PC. `HOT_FN` (in `config.h`) puts hot
functions in IRAM only when `ESP_PLATFORM` is defined, without including any
ESP-IDF header.

### Tasks / cores (sand mode)

| Task | Core | Rate | Work |
|---|---|---|---|
| `sim` | 0 | `SIM_HZ` (120 Hz), own clock | read IMU state, apply material, fixed sim steps, build snapshot |
| `app` (render) | 1 | up to `TARGET_FPS` | button / look switch, render snapshot → frame buffer, DMA kick, LED, stats |
| `imu` | 1 | `IMU_POLL_HZ` (200 Hz), prio above `app` | I2C read, mapping, filtering, shake detection |

The IMU task (and, because it installs the I2C driver, the I2C interrupt)
moved to core 1 on 2026-10-26 so it no longer preempts the simulation, which
has the tight 8.33 ms budget; rendering uses a small part of each frame.
| button | esp_timer | 10 ms | debounce + press classification |

`tiltdot` and `testpattern` run single-threaded in the `app` task.

The second core became necessary after measuring (§10): even optimised,
a step with every grain moving is estimated at ~3–4.5 ms on the S3, above the
1.5 ms needed to fit two steps plus rendering into one 60 fps frame on one
core.

### Sim ↔ render hand-over

```
sim task (core 0)                         render task (core 1)
  every 1/SIM_HZ:                           every frame:
    gravity, shake, respawn request           button: respawn request / palette
    sim_step() × due                          take newest snapshot (if any)
    if renderer took the last one:            wait for previous DMA
      render_snapshot() → back                render_frame(front) → frame buffer
      swap(back, ready); fresh = 1            start DMA push, LED, stats
```

Snapshots are triple-buffered GRID_W × GRID_H byte maps (one byte per cell:
6-bit brightness | 2-bit shade; 0 = empty). Publishing swaps back↔ready,
taking swaps ready↔front; each swap is a pointer exchange under a
`portMUX` spinlock, so neither task ever waits and neither touches a
buffer the other owns. Nothing else of the sim is shared: respawn is an
atomic flag, statistics travel under their own spinlock.

Real time: physics advances on its own clock and never waits for drawing.
If rendering is slow, frames are dropped (the renderer just shows the newest
snapshot). Only if one step costs more than 1/SIM_HZ can the sim fall
behind; it then keeps at most `SIM_MAX_BACKLOG_MS` of backlog, drops the
rest and logs `SIM BEHIND REAL TIME`.

### Frame push

Single full-frame buffer (172×320×2 = 110 080 B) in DMA-capable internal RAM.
`esp_lcd` splits it into chunks of `max_transfer_sz`, all queued at once, so the
whole push is asynchronous; a semaphore from `on_color_trans_done` signals the
end.

### Memory placement

| Data | Size (GRAIN_PX 2) | Where |
|---|---|---|
| frame buffer | 110 080 B | internal, DMA-capable |
| sim (grains, cells, shades, idle) | 109 240 B | internal (falls back to PSRAM with a loud warning) |
| snapshots | 3 × 13 760 B | PSRAM (sequential, cache-friendly; shared cache keeps cores coherent) |
| glow buffer, colour LUTs | ~14 KB static | internal |

The banner reports where the sim and snapshots actually went. Code runs from
flash through the 16 KB instruction cache; the hot loops (`sim_step` ≈ 2 KB,
`render_frame` < 1 KB) stay resident, so they are not moved to IRAM, which
would cost scarce internal RAM.

## 5. Simulation

Hybrid particle + grid (approach of Adafruit PixelDust, independent implementation).

- Grid: `GRID_W = 172 / GRAIN_PX`, `GRID_H = 320 / GRAIN_PX`
  (86×160 for `GRAIN_PX = 2`; 172×320 for 1). One grain per cell max.
  Cell stores grain index + 1 (0 = empty), `uint16_t`.
- Grain: float position (cell units), float velocity (cells/step), shade index.
- Grain count = `FILL_FRACTION × cells` (default 0.33).
- Fixed timestep `SIM_HZ` (120 Hz) on its own task and clock, decoupled
  from rendering (see §4).
- **Per-grain code uses no software maths.** The S3 FPU has hardware
  add/multiply but no divide or square root, and `sqrtf`/`floorf`/`ceilf`/`/`
  compile to library calls costing hundreds of cycles. The step uses an
  integer-cast floor, a bit-trick `rsqrt` with one Newton step (~0.2 %),
  a reciprocal table for the sub-step count, and folds the motion total into
  the update (previously a separate `sqrtf` over every grain every step).
  Verified by disassembly: `sim_step`, `render_snapshot` and `render_frame`
  call nothing but `memset`/`memcpy`.
- Deterministic xorshift RNG seeded by the app.

### Inputs: gravity model

`sim_set_gravity(gx, gy, gz)` in g units, screen frame (x right, y down,
z out of the screen), low-passed by the IMU module. All three components
matter and the behaviour follows the physics of sand in a thin box behind
glass:

- **Driving force = in-plane projection, not normalised.**
  `a = (gx, gy) · GRAVITY_SCALE`. Upright (picture-frame) gives the full 1 g
  toward the bottom; leaning back by θ weakens it to cos θ; tipping past flat
  with the top edge down reverses it, so sand runs to the top; rotating in the
  plane steers it.
- **Glass friction = screen-normal component.** `glass = BACK_FRICTION·|gz|`
  (g). Coulomb friction: each step a moving grain's speed drops by
  `glass · GRAVITY_SCALE`; at or below that it stops. Upright there is no glass
  friction; flat it is largest.
- **Frozen when friction wins.** The pile only gets a "down" direction
  (support test, resting state, repose rule) when
  `net = |g_xy| − glass > GRAVITY_DEADZONE`. Otherwise it holds its shape,
  so a board lying flat stays put despite sensor noise. With defaults this is
  the last ~8.5° before flat (lean > ~81.5° from vertical).
- **Steeper piles when leaned back.** A grain on a slope θ is held by
  internal friction (`tan(repose) · |g_xy| cos θ`) plus glass friction:
  `tan θ_max = tan(REPOSE_ANGLE_DEG) + glass / (|g_xy| · cos(REPOSE_ANGLE_DEG))`.
  Upright this is just `REPOSE_ANGLE_DEG`.
- **Slower avalanches under a weaker pull.** The repose-rule probability is
  `REPOSE_PROB · min(1, sqrt(net))` (fall time scales with 1/√g).

### Per-step update

Processing order alternates forward/backward and starts at a random offset
every step (no directional bias).

**Sleeping grains.** A grain that hasn't moved for `SLEEP_STEPS` (64) steps
is skipped (a one-byte compare) until something can change its situation:
a grain leaving a cell within `WAKE_RADIUS` (2) of it, a shake, a respawn, a
material change, or gravity having moved far enough from where it was when
the grains last went to sleep (`ref_g`, recorded at the last wake):

- the snapped direction or the frozen state changed, or
- the in-plane direction turned by more than `WAKE_ANGLE_DEG` (2°), or
- the in-plane or the normal magnitude changed by more than
  `WAKE_PULL_DELTA_G` (0.06 g).

Because it compares against the reference rather than the previous frame,
slow drift still wakes the pile after 2°, while tremor around one attitude
never does. A frozen pile (lying flat) is never woken by gravity noise at
all. (Until 2026-10-26 the rule was a 0.02 g vector distance, which hand
tremor crossed constantly: a held, settled pile showed 4540 awake.) A
sleeper count lets moving grains skip the neighbourhood scan entirely when
nothing is asleep (the worst case). Radius 1 / 24 steps left piles up to
4.5° steeper than without sleep; 2 / 64 matches the no-sleep results
within 0.8° on every host test.

For each awake grain:

1. **Support test.** Snap gravity direction to the nearest of 8 grid
   directions `D`. The grain is *supported* if the cell at `c + D` is a grain
   or a wall.
2. **Resting** (supported and speed < `REST_SPEED`): velocity is zeroed
   (static friction). With probability `REPOSE_PROB · min(1, √net)` the
   **repose rule** may move it one cell (see below). Done.
3. **Moving:**
   - `v = (v + a) · (1 − DAMPING)`; grains already faster than `REST_SPEED`
     get uniform jitter `±JITTER`.
   - If supported, scale the velocity component tangential to `D` by
     `(1 − CONTACT_FRICTION)` (sliding friction).
   - Apply the back-face Coulomb friction, clamp `|v| ≤ MAX_SPEED`.
   - Move in sub-steps of ≤ 1 cell. For each sub-step:
     target cell free → move;
     else try x-only / y-only (larger velocity component first), reflecting the
     blocked component with `−ELASTICITY`;
     else reflect both and stop.

### Repose rule (the important part)

Work in the gravity frame: `D` = down, `S` = one of the two lateral
directions (perpendicular to `D`, chosen randomly first).

Measured slope toward `S` over lateral baselines k = 1…`REPOSE_SCAN`:

```
drop_k = number of free cells straight "down" from (c + k·S), capped at REPOSE_SCAN
slope  = max_k atan(drop_k / k)
```

The true gravity direction is tilted by φ from `D` (|φ| ≤ 22.5° because of the
8-way snap). If gravity leans toward `S` by φ, the slope that matters is
`slope + φ`. Slide condition:

```
base    = REPOSE_ANGLE_DEG − (D diagonal ? REPOSE_DIAG_CORRECTION_DEG : 0)
base    = atan(tan(base) + glass / (|g_xy| · cos(base)))     (glass friction, see Inputs)
slide if  drop_k > k · tan(base − φ_S) · (1 ± REPOSE_DITHER)  for some k
```

Along diagonals the discrete scan under-measures drops by about one cell; the
4° correction was calibrated with `tools/sim_host.c`. The ±15 % random dither
on the threshold lets the settled slope fall between the few slopes the grid
can represent (2/3, 3/4, 1 …); without it, leaned-back piles snapped to 45°.

If it holds, move to `c + D + S` if free (diagonal slide), otherwise to `c + S`
(lateral creep) if free. Using baselines up to 3–4 cells lets the discrete
grid represent slopes like 2/3 (33.7°) and 3/4 (36.9°) instead of only 45°,
so piles settle near `REPOSE_ANGLE_DEG` (default 33°). The φ correction keeps
the behaviour consistent when the 8-way snap switches frames, so the avalanche
point under tilt tracks the repose angle.

Measured on PC with `tools/sim_host.c` (86×160 grid, defaults). "Lean" is
the angle the board is leaned back from upright; "rotate" is rotation in the
screen plane after the sand settled upright. Expected heap angle from the
formula above: 33.0° / 37.0° / 43.8° / 52.8° at lean 0/30/60/75°.

| Check | Result |
|---|---|
| Heap, upright (lean 0) | 33.1° (172×320 grid: 33.6°); spawn settles in 4.3 s |
| Heap, lean 30° | 33.3° (grid can't show 37°: slopes jump 33.7° → 36.9° → 45°); 4.7 s |
| Heap, lean 60° | 43.5°; spawn settles in 6.3 s |
| Heap, lean 75° | 52.4°; spawn settles in 11.2 s |
| Upright, rotate 20/30° | surface stays put |
| Upright, rotate 40/50/60/70° | avalanches; effective repose 28.4/30.9/28.9/33.1° |
| Lean 60°, rotate 20/30/40° | surface stays put (38.6° still holding) |
| Lean 60°, rotate 50/60/70° | avalanches; effective repose 38.4/41.1/42.9° |
| Lean 70–78°, rotate 90° | flows (443…288 grains moved) |
| Lean 80°, rotate 90° | barely (47 grains); lean ≥ 82°: 0 — friction holds |
| Lying flat, ±0.03 g noise, 2000 steps | 0 grains moved |
| Tipped past flat, top edge down (lean 120°) | mean grain y 133.6 → 26.3: sand runs to the top |
| Shake then resettle | motion back to 0 within 1500 steps |

### Shake

`sim_shake(dir_x, dir_y, strength)`: every grain gets an impulse
`dir · strength · U(0.5, 1) + isotropic random · strength · 0.5`, which takes
it out of the resting state; it then resettles under gravity.

### Outputs

Per-grain speed (for brightness), total motion (sum of speeds, for the LED and
stats).

### Classic physics v1 (frozen reference)

Recorded on 2026-10-26, before the material system was introduced. This is
the behaviour confirmed on hardware ("looks and feels great"); material
CLASSIC must reproduce it exactly. Algorithm as described in §5 above
(support test, resting state, repose rule with glass-friction term and
dither, PixelDust-style moving step with axis-by-axis collision, Coulomb
glass friction, sleeping grains). Parameter values:

| Parameter | Value | Meaning |
|---|---|---|
| `SIM_HZ` | 120 | fixed steps per second |
| `GRAVITY_SCALE` | 0.06 | cells/step² per g of in-plane gravity |
| `DAMPING` | 0.01 | velocity lost per step while moving |
| `ELASTICITY` | 0.30 | velocity kept (reversed) on a blocked axis |
| `MAX_SPEED` | 2.0 | cells/step; sub-stepped above 1 |
| `JITTER` | 0.01 | ± cells/step on grains faster than `REST_SPEED` |
| `CONTACT_FRICTION` | 0.30 | tangential velocity lost per step when supported |
| `REST_SPEED` | 0.15 | below this a supported grain is resting |
| `BACK_FRICTION` | 0.15 | glass friction per g of \|gz\| |
| `GRAVITY_DEADZONE` | 0.03 g | net pull below this freezes the pile |
| `REPOSE_ANGLE_DEG` | 33.0 | upright angle of repose |
| `REPOSE_PROB` | 0.35 | × min(1, √net): chance per step a resting grain tries to slide |
| `REPOSE_SCAN` | 4 | lateral/vertical cells scanned for the slope |
| `REPOSE_DIAG_CORRECTION_DEG` | 4.0 | threshold reduction in diagonal gravity frames |
| `REPOSE_DITHER` | 0.15 | ± fraction on the slide threshold |
| repose slide token velocity | 0.8 × `REST_SPEED` | velocity given to a grain after a repose move |
| `SLEEP_STEPS` | 64 | steps without moving before a grain sleeps |
| `WAKE_RADIUS` | 2 | cells around a vacated cell whose sleepers wake |
| `WAKE_GRAVITY_DELTA_G` | 0.02 g | wake-all when gravity moved this far (vector) since the last wake |
| `SHAKE_THRESHOLD_G` | 0.60 g | \| \|a\| − 1 g \| above this is a shake |
| `SHAKE_STRENGTH` | 0.9 | cells/step impulse per g of shake excess |
| `IMU_POLL_HZ` / `IMU_LPF_ALPHA` | 200 / 0.15 | gravity low-pass (~30 ms) |
| `FILL_FRACTION` / `GRAIN_PX` | 0.33 / 2 | 4540 grains on 86×160 |
| `GLOW_BASE` / `GLOW_SPEED_GAIN` | 150 / 140 | settled brightness / per cell/step of speed |
| `PERSISTENCE_KEEP` | 0.60 | afterglow kept per frame |

Golden reference: `tools/sim_golden.c` holds state hashes (cell grid +
every grain's position/velocity bits) for fixed-seed scenarios captured from
this exact code; CLASSIC must reproduce them bit for bit on the host.

`WAKE_GRAVITY_DELTA_G` is the one v1 value since replaced (by the wake rule
under "Sleeping grains"); it only decides when sleepers are re-checked, and
the golden scenarios wake under both rules, so they still match bit for bit.

## 5b. Materials

All per-material physics is a `material_t` in `config.h`
(`MATERIAL_CLASSIC_PARAMS`, `MATERIAL_WATER_PARAMS`); `material.c` holds the
table. One engine; the material's *resting rule* decides what a supported,
slow grain does: `MAT_REST_REPOSE` (sand) or `MAT_REST_SPREAD` (water).
`sim_set_material()` switches at any time: every grain stays where it is,
all are woken and the gravity-derived terms are recomputed.

| Field | CLASSIC (= v1) | WATER | Meaning |
|---|---|---|---|
| `gravity_scale` | 0.06 | 0.06 | cells/step² per g |
| `damping` | 0.01 | 0.005 | drag per step |
| `elasticity` | 0.30 | 0.05 | bounce on a blocked axis |
| `max_speed` | 2.0 | 2.0 | cells/step cap |
| `jitter` | 0.01 | 0.01 | random kick on fast grains |
| `contact_friction` | 0.30 | 0.02 | sliding friction on the pile |
| `rest_speed` | 0.15 | 0.15 | resting threshold |
| `glass_friction` | 0.15 | 0.08 | Coulomb friction per g of \|gz\| |
| `rule_token` | 0.8 | 0.8 | × rest_speed, velocity after a repose slide |
| `rest_rule` | REPOSE | SPREAD | resting behaviour |
| `rule_prob` | 0.35 | 1.0 | × min(1, √net) per step |
| `repose_angle_deg` / `_scan` / `_diag_correction_deg` / `_dither` | 33 / 4 / 4 / 0.15 | — | angle of repose |
| `spread_scan` / `spread_push` / `spread_step` | — | 24 / 0.8 / 0 | look-ahead, sideways push (cells/step), jump |
| `level_rate` / `level_range` | 0 (off) | 8 / 86 | levelling transfers per step, reach |
| `level_max_motion` | — | 400 | levelling paused while total motion is above this (splashing) |
| `wake_radius` | 0 (= `WAKE_RADIUS` 2) | 1 | cells around a vacated cell whose sleepers wake |
| `shade_spread` | 1.0 | 0.3 | palette shade variation |
| `name` (overlay / logs) | "gravel" | "water" | user-facing name; internally still `MATERIAL_CLASSIC` |

**CLASSIC verification** (host, x86-64 gcc -O2): `tools/sim_golden.c`
matches all 8 v1 state hashes bit for bit, and `tools/sim_host.c` output is
identical to v1 in every line (heap angles, lean, rotation, flat, tip, shake).
On the ESP32-S3 the compiler may fuse multiply-adds differently now that the
constants are struct fields, so the board can differ from v1 in the last
float bit, not in behaviour.

### Water model

1. Same moving step as sand, with low bounce and almost no friction on the
   pile, so it slides and sloshes, and splashes on shake.
2. **Spread rule** (resting surface grains): look up to `spread_scan` cells
   along each lateral direction for a spot where the grain would end at least
   one cell lower in *true* gravity (a drop below that spot, or gravity
   leaning toward that side by the residual tilt φ). If found, give the grain
   a sideways velocity of `spread_push`; it slides with momentum and rests
   again when nothing lower is in reach.
3. **Levelling pass** (stand-in for pressure): in a one-grain-per-cell grid
   only the surface layer can flow, so a deep pool would take 5–10 s to level
   after a big tilt (measured: every surface-only variant tried — pushes,
   jumps, higher speed caps — needed 5–18 s). Each step, while any water
   grain is awake, the grid is split into lines along gravity; up to
   `level_rate` top grains move from the highest line to the free spot above
   the lowest line within `level_range`, only if that lowers the grain by at
   least half a cell (so it always converges). Heights use true gravity, so
   tilt inside the snapped frame is honoured. Water's shades are pulled
   together (`shade_spread` 0.3), so this reads as one side of the surface
   dropping while the other rises.
4. Glass friction and the frozen state work as for sand: lying flat, water
   holds still; it freezes at ~84° lean (lower glass friction than sand's
   ~81°).
5. **Idle cost (2026-10-26, second round).** Water differs from gravel in
   three places that only affect cost, not the look:
   - A gravity change wakes only *exposed* water grains (a free cell beside,
     above or below), not the whole pool: a fully surrounded grain cannot move
     whatever gravity does, and wakes through its neighbours if one leaves.
     Gravel keeps wake-all, so CLASSIC stays bit-identical.
   - A resting water grain with both sideways cells taken skips the spread
     look-ahead and its random draw (the whole submerged body).
   - The levelling pass is skipped while it cannot find anything new: the
     last pass ended level, no grain changed cell since, and gravity turned
     less than `LEVEL_RECHECK_DEG` (0.5°). It is also paused above
     `level_max_motion` (hard splashing), and no longer requires an awake
     grain to run.
   - `wake_radius` 1 instead of 2 (gravel keeps 2; radius 2 exists for the
     repose look-ahead, which water does not have). With interior grains now
     staying asleep, radius 2 made every moving water grain scan 25 cells.
6. Tilts that stay inside one snapped gravity frame (< 22.5°) are re-levelled
   by levelling transfers (~3–8 grains/step) rather than by visible spread
   pushes; bigger tilts flow physically. Unchanged from the previous round.

### Material host results (`tools/sim_water.c`)

| Test | Result |
|---|---|
| Water spawn | settles in 4.4 s, surface within 1 cell |
| Tilt 0 → 45° | surface 40° at 0.75 s, 42° at 1.25 s, 43.7° (deviation 3.5 cells) from 4 s; at rest 5.9 s |
| Back 45° → 0° | level within 2 cells at 0.85 s, within 1 cell at 1.25 s, all asleep at 1.75 s |
| Classic heap (28-cell high) → water | within 2 cells at 0.35 s, 1 cell at 0.8 s |
| Water pool → classic | 0 grains move, 0 awake; next rotation of 50° piles at effective repose 31.3° (sand alone 31.0°) |
| Water lying flat, ±0.03 g noise | 0 grains moved |
| Held upright, ±0.03 g tremor (± 1° sway) | 0 of 4540 awake, both materials (v1 rule: 4540) |
| Deliberate 5° tilt during tremor | whole pile awake within 8 steps (67 ms), both materials |

At 45° (a diagonal gravity frame) the levelled surface sits within ~1.9
height units, which shows as 43.7° instead of 45° and 3.5 cells deviation
across the width; upright it levels to 1 cell.

### Water idle round (2026-10-26)

**Hardware before:** water held in hand ~7 ms/step mean, 8.5–9 ms max,
4540 awake with motion 0–11 for long stretches; gravel held 4 ms / 6–7 ms
(accepted as is); both 0 awake, 0.53 ms lying still. Render "fps 60 (40 new)",
sometimes 20–30 new, with the sim at 120 steps/s. One dip to 107 steps/s in a
hard water shake.

**Root cause, idle water.** `tools/sim_hand.c` (lean 20°, ±0.03 g tremor,
±3° + ±2° in-plane sway, ±2° lean wobble) reproduces it: the hand motion
crosses the 2° / 0.06 g wake limits about twice per second; every crossing
woke all 4540 grains, and they need 64 steps to fall asleep again, so on
average ~3250 were awake (gravel the same: accepted). For water each awake
resting grain also ran the spread look-ahead every step (rule_prob 1.0) and
the levelling pass rescanned the grid every step. Levelling transfers
(50–80/s following the sway) were a small part; spread pushes were 0.

**Fix:** item 5 of the water model above. Hand-held host results:

| | before | after |
|---|---|---|
| water, held: awake (20 s mean) | 3254 | 73 |
| water, held: host µs/step mean | 90.5 | 12.6 |
| gravel, held (unchanged) | 3240 awake, 64 µs | same |
| water, 5° tilt during tremor | — | first grains move after 4 steps (33 ms); surface only wakes (peak 160) |

**Root cause, "new" frames.** The sim published a snapshot only at the end of
a batch of steps, and only if the renderer had taken the previous one. When
steps ran near 7–9 ms the sim fell behind and ran two steps back to back
(14–18 ms); the gap between publishes then exceeded the 16.7 ms frame and
frames found nothing new. A timing model (`fps 60`, ±1 ms jitter) reproduces
it: 88 % new frames with 5–9 ms steps, 15 % with 7–9.5 ms.

**Fix:** the renderer records when it takes a snapshot; after *every* step
the sim publishes (or overwrites the waiting one) if the next frame comes
before another step can finish (`SNAP_MARGIN_US` 3 ms of slack), or if a
frame is already due and nothing is waiting. Each frame gets the state of
the last step before it. Model: 99–100 % new frames for 0.5–9.5 ms steps, at
60–100 snapshot builds/s (was ≤ 60; each is a few hundred µs on the sim
core, shown in the stats as "snapshot").

**Water cost** (host µs/step mean / worst; `tools/sim_bench.c`):

| Scenario | previous round | now |
|---|---|---|
| awake (all woken, resting) | 123.3 / 231.0 | 50.1 / 124.7 |
| steer (1°/step) | 220.8 / 495.7 | 189.9 / 388.7 |
| slosh | 154.4 / 304.6 | 96.4 / 204.8 |
| shake | 252.2 / 454.5 | 241.9 / 483.9 |
| flow | 75.3 / 207.0 | 52.6 / 154.4 |
| freefall | 264.1 / 396.1 | 248.1 / 333.5 |

Board estimate. Two ratios fit the hardware data: resting/awake work is
~65–70× the host (0.53 ms ↔ 8 µs lying still; 8.6 ms ↔ 123 µs all-awake
water), moving work ~14× (gravel shake worst 6–7 ms ↔ 461 µs). Water held in
hand: ~0.9 ms/step (was ~7). All-awake bursts: ~3.5 ms (was ~8.6). Hard
shake: mean ~3.4 ms, single worst ~6.8 ms, the same as gravel.

**Water look**, previous round → now (`tools/sim_water.c`): 45° tilt surface
40.1° at 0.75 s in both, 43.7°/3.5 cells final in both, at rest 5.9 s → 5.0 s;
back to upright level in 0.85 s in both; heap → water within 2 cells at 0.35 s
in both, final 1 → 2 cells; water → gravel then 50°: effective repose 31.3° →
31.1°; lying flat 0 moved in both (145 → 2 awake); freeze point ~84° in both.

## Mode-name overlay

On a BOOT long press, and once at boot, the material name of the new look
("GRAVEL", "WATER") is drawn centered in the palette's brightest shade
(`palette_brightest()`) in a built-in 5×7 terminal font scaled ×4
(`OVERLAY_SCALE`, reduced automatically if it would not fit; "GRAVEL" is
140 px wide), over a backing box `OVERLAY_PAD` (6) px larger than the text.
Full strength for `OVERLAY_HOLD_MS` (600 ms), then box and text fade out
together over `OVERLAY_FADE_MS` (400 ms): the box darkens the grains behind it
by alpha and the text is added on top. Rendering only (core 1, after
`render_frame()`, ~6 k pixels); the simulation keeps running. Looks are
named "green gravel", "orange gravel", "green water", "orange water".

## 6. IMU

- QMI8658 accel ±8 g, 250 Hz ODR, on-chip LPF off (filtering is done in
  software so shake detection sees the raw signal). CTRL1 auto-increment and
  CTRL7 = 0x43 exactly as in the Waveshare demo (gyro runs but is unused).
- Probes 0x6B, falls back to 0x6A; warns if WHO_AM_I ≠ 0x05.
- Polled at `IMU_POLL_HZ` in its own task.
- Axis mapping in `config.h`: for screen X, Y and Z choose source axis (0/1/2)
  and sign. Gravity = −acceleration (accelerometer at rest reads +1 g up).
- **Hardware results (tiltdot):**
  - WHO_AM_I = 0x05 at address 0x6B. Confirmed.
  - Board held upright like a picture frame, portrait, USB at the bottom:
    raw ay ≈ −0.9 g, az ≈ 0. So chip Z is the screen normal and chip Y is
    screen Y with sign +1: the dot falls to the bottom upright and to the top
    when rotated 180°. Confirmed.
  - Rotating in the screen plane swings raw ax between about −0.7 and +0.7 g;
    with sign +1 the dot moved *away* from the lowered edge. No axis swap is
    needed, only the X sign: now `IMU_SCREEN_X_SIGN = −1` (to be re-checked).
- Low-pass `IMU_LPF_ALPHA = 0.15` at 200 Hz (≈ 30 ms time constant), applied
  to each component of the gravity vector — never to an angle, so there is
  no wraparound when steering through ±180°.
- Shake detection uses the raw 3-axis magnitude (unchanged).
- tiltdot drives the dot with the same model (in-plane pull + glass Coulomb
  friction) and prints, 10× per second: raw chip axes, screen gravity,
  in-plane angle (`atan2(gx, gy)`: 0° = toward bottom edge, +90° = right,
  −90° = left, ±180° = top), in-plane magnitude, glass friction and net pull,
  with "(held by friction)" when the sand would be frozen.
- Shake: `| |a| − 1 g | > SHAKE_THRESHOLD_G` → shake level = excess;
  direction = high-passed acceleration (a − lowpass) projected to screen.
  Consumed once per frame by the app.

## 7. Look

- Black background. Palettes in `palette.c`, 4 shades each:
  - **green** (`PALETTE_GREEN`, P1): `#33FF33`, `#2BD92B`, `#5CFF5C`, `#22B822`; LED `#00FF10`
  - **orange** (`PALETTE_ORANGE`, P3-style amber): `#FFB000`, `#E69A00`, `#FFC233`, `#CC8800`; LED `#FF6000`
  - (The deeper `#FF6A00` gas-plasma palette was tried and removed: too dark
    on hardware. "Orange" is the former amber, values unchanged.)
- Each grain picks a random shade at spawn → textured piles. The material's
  `shade_spread` scales the variation (classic 1.0; water 0.3 for a smoother
  body); the grain keeps its shade, so switching back restores the texture.
- Brightness: settled grains at `GLOW_BASE`, moving grains up to 255, scaled by
  speed (`GLOW_SPEED_GAIN`); the brightest levels blend toward white
  (`HOT_WHITE_MIX`) like an overdriven phosphor.
- Phosphor persistence (`PERSISTENCE_ENABLE`, default on): a per-cell glow
  buffer (6-bit level | 2-bit shade in one byte) decays by
  `PERSISTENCE_KEEP` per frame instead of clearing, so moving sand leaves a
  short afterglow. Off → glow cleared each frame.
- Speed → brightness comes from a 256-entry table indexed by speed², so no
  square root per grain.
- Colour lookup: `lut[palette][shade][level>>2]` pre-computed RGB565
  (in panel byte order, see `BOARD_LCD_SWAP_BYTES`), rebuilt on palette change.
- Sand-mode looks come from `SAND_MODE_TABLE` in `config.h`, a list of
  (name, palette, material) entries; the first is the boot default. Default
  table: green gravel → orange gravel → green water → orange water → back
  to the start. BOOT long press steps through it; every grain stays where it
  is, only colour and behaviour change (a settled sand pile slumps flat as
  water; water switched to sand rests and piles on the next tilt). The mode
  name is logged on each switch and shown in the banner. tiltdot uses
  `PALETTE_DEFAULT` (green).
- RGB LED: palette's primary colour, slow breathing pulse whose intensity
  follows total sand motion.
- **LED state per mode.** `led_init()` is the first thing `app_main` does and
  clears the LED (off), so nothing latched by a previous firmware or line
  noise survives. If the RMT driver fails, GPIO38 is driven low. Then:
  `testpattern` → pulsing pure red; `tiltdot` → palette LED colour, dim
  pulse, brighter while the dot rolls; `sand` → palette LED colour, pulse
  brightness following sand motion, changes with the palette. Between reset
  and `app_main` (ROM + bootloader, ~0.3 s) the LED is not under our control.
- Backlight: `BACKLIGHT_PERCENT` via LEDC PWM.

## 8. Controls

| Input | Action |
|---|---|
| BOOT short press (< `BTN_LONG_MS`) | respawn sand scattered in the top part of the screen |
| BOOT long press (≥ `BTN_LONG_MS`, fires while held) | next sand-mode look: green gravel → orange gravel → green water → orange water |

## 9. Bring-up modes

`APP_MODE` is set per PlatformIO environment (`build_flags` in
`platformio.ini`): `-e testpattern`, `-e tiltdot`, `-e sand` (default).
`config.h` only supplies a fallback (`MODE_SAND`) when the macro is not
defined.

Every mode prints a boot banner (mode, firmware version, ESP-IDF version,
display settings, IMU address + WHO_AM_I, LED/button init result, free
internal/PSRAM heap). Because the USB-Serial-JTAG port is usually opened after
boot output is gone, the banner is re-printed whenever
`usb_serial_jtag_is_connected()` goes from false to true.

| Mode | Purpose |
|---|---|
| `MODE_TEST_PATTERN` | 8 colour bars (W,Y,C,G,M,R,B,K), 1-px white border, red square top-left, green square top-right, blue bottom-left. Confirms offset (all four border edges visible), orientation, inversion (black is black) and colour order. RGB LED pulses pure red (checks LED colour order). Prints an `alive` line once per second. |
| `MODE_TILT_DOT` | one dot rolls downhill; raw accel (g) and mapped gravity printed at 10 Hz. Use to fix axis mapping/signs. |
| `MODE_SAND` (default) | full demo |

## 10. Performance

Target: 60 fps with heavy motion and physics in real time (120 steps/s).

**Display.** A full frame at 80 MHz is 110 080 B × 8 / 80 MHz ≈ 11 ms of DMA,
overlapped with everything else. Clean at 80 MHz during continuous redraws
(hardware).

**Hardware before this round** (single core, GRAIN_PX 2, 4540 grains): render
3.8 ms/frame; sim 5 ms/step at rest and up to 10.6 ms/step in motion;
the loop hit its 4-steps/frame cap and dropped to ~21 fps with slow-motion
physics.

**Root cause.** Not optimisation level (`-O2` confirmed in the real compile
commands, zero `-Wdouble-promotion` warnings) and not the RNG (inline
xorshift32 seeded once from `esp_random()`). The per-grain code called
software maths: one `sqrtf` per grain per step for the motion total
(4540/step even at rest), plus per moving grain two `sqrtf`, two float
divides (`__divsf3`), `floorf`×2 per sub-step, `ceilf` and `fmaxf`. Render
also did one `sqrtf` per grain per frame. A cost model of ~0.75 µs per
`sqrtf`, ~0.3 µs per divide and ~0.15 µs per floor/ceil, on top of ~0.3 µs
(resting) / ~0.9 µs (moving) of plain work per grain, reproduces both measured
figures (5 ms at rest, ~10 ms with ~3000 moving).

**Host benchmark** (`tools/sim_bench.c`, µs/step on the PC; the PC has
hardware sqrt/divide, so it understates the board gain of stage A):

| Scenario | grains moving | before | A: no soft maths | B: + sleep (final) |
|---|---|---|---|---|
| settled | 0 | 82.5 | 77.0 | 6.6 |
| freefall | 3580 | 270.9 | 233.3 | 245.7 |
| shake (worst) | 4255 | 350.1 | 316.5 | 323.0 |
| flow (avalanche) | 145 | 91.5 | 84.5 | 37.6 |

Software maths calls per step: before 4540 `sqrtf` at rest; 8.8 k `sqrtf`,
9.7 k `floorf`, 4.3 k `ceilf`, 4.3 k `fmaxf` and ~8.5 k divides when shaking.
After: 0 in every scenario.

**Board estimate (same cost model; to be measured):**

| Scenario | before (measured / model) | after (estimate) |
|---|---|---|
| settled | 5 ms | ~0.1–0.2 ms |
| avalanche | — / ~1.5 ms | ~0.2–0.4 ms |
| freefall | — / ~12 ms | ~2.5–3.5 ms |
| shake, everything moving | 10.6 ms / ~14 ms | ~3–4.5 ms |
| render per frame | 3.8 ms | ~0.5–1 ms (+ ~0.3 ms snapshot on the sim core) |

Everything-moving still misses 1.5 ms/step on one core, hence the second core:
the sim core needs ≤ 8.3 ms per step for real time, so the worst case runs at
~40–55 % load, while the render core needs ~1 ms per frame plus the
overlapped 11 ms DMA → 60 fps.

**Stats** (once per second): fps and how many frames had a new snapshot; sim
steps/s (120 = real time), mean and max µs per step, awake grains; snapshot,
render and DMA-wait µs; motion; `SIM BEHIND REAL TIME` if any sim time was
dropped.

### Materials round (2026-10-26)

**Hardware, previous build:** 120 steps/s and 60 fps including shaking; sim
in internal RAM. Held in hand and settled: 4540 awake, ~4.4 ms/step (tremor
crossed the 0.02 g wake threshold). Lying still: ~500 awake, 1.07 ms. Heavy
motion: 4–6 ms/step mean, ~9 ms max (over the 8.33 ms period; steps/s
dipped to 116–117).

**Changes for the budget:** the wake rule (tremor no longer wakes a held
pile, flat never wakes); exact-result speedups in the classic path
(two-compare bounds check, cheaper floor); hot functions in IRAM
(`HOT_FN`: `sim_step` with its inlined helpers, `wake_around`,
`render_frame`, `render_snapshot`); the IMU task and its I2C interrupt moved
off the sim core.

**Host benchmark** (`tools/sim_bench.c`, µs/step mean / worst, best of 5):

| Scenario | v1 classic | classic now | water |
|---|---|---|---|
| settled | 6.9 / 28.5 | 6.7 / 32.2 | 7.0 / 25.5 |
| freefall (3580–3730 moving) | 242.8 / 337.0 | 206.6 / 308.8 | 240.2 / 361.7 |
| shake (worst case) | 322.3 / 541.1 | 257.3 / 460.6 | 249.2 / 482.5 |
| flow (turn 90°) | 37.9 / 145.2 | 35.9 / 123.7 | 72.6 / 219.5 |
| slosh (±45° every 0.5 s) | 91.6 / 194.9 | 87.4 / 177.6 | 147.0 / 302.8 |
| steer (1°/step, nothing sleeps) | 157.5 / 356.9 | 142.9 / 383.8 | 208.1 / 424.5 |

**Board estimate:** the hardware figures for v1 (4–6 ms mean, ~9 ms max in
heavy motion) are ~17× the host v1 shake numbers. On that ratio the worst
step becomes ~7.8 ms classic and ~8.2 ms water before the IRAM and IMU-core
changes, which the host cannot measure; they remove flash-cache misses
shared with the render core and 200 Hz preemption of the sim task. Expected
worst case therefore ~6.5–7.5 ms for both materials — at or slightly above
the 7 ms target. Held in hand, settled: ~0.1–0.2 ms (was 4.4 ms).

If the board still shows worst steps above ~7 ms, the remaining levers all
change behaviour, so they are left for a decision: fewer grains
(`FILL_FRACTION`), a lower water `max_speed` or `level_rate`, or capping
how often shake impulses are applied.

## 11. Decisions and deviations

- **Panel init.** Uses ESP-IDF's built-in `esp_lcd_new_panel_st7789` for
  reset/sleep-out/MADCTL/COLMOD/draw, then sends Waveshare's vendor register
  sequence (RAMCTRL, porch, gate, VCOM, power, gamma) from [ARD-DEMO]
  `Display_ST7789.cpp`, then INVON, mirror and gap through the esp_lcd API.
- **Pixel byte order (resolved on hardware).** The first flash used
  big-endian pixels. The test pattern then showed red→blue, green→red,
  blue→green (yellow→magenta, cyan→yellow, magenta→cyan, white and black
  unchanged) and the grey ramp broke into magenta/green/white segments. That
  is exactly what swapping the two bytes of each RGB565 word does (e.g. red
  0xF800 read as 0x00F8 is mostly blue), while a BGR/RGB mix-up would only
  exchange red and blue and leave green alone. So the panel reads pixels
  little-endian: the RAMCTRL ENDIAN bit that both demos set (`00 E8`) does
  apply over SPI, contrary to the datasheet note I relied on.
  `BOARD_LCD_SWAP_BYTES` is now 0.
- **Two Waveshare demos disagree.** [IDF-DEMO] (`Vernon_ST7789T.c`) uses
  different VCOM/gamma values, ends with MADCTL = BGR | MX (0x48), runs SPI
  at 12 MHz and feeds LVGL colours without byte swap. [ARD-DEMO] uses
  MADCTL = 0x00, 80 MHz and sends big-endian (`PNG_RGB565_BIG_ENDIAN`) pixel
  bytes. We follow the Arduino demo for init values, MADCTL and clock. For
  byte order the hardware sided with the ESP-IDF demo (see above).
  `BOARD_LCD_BGR`, `BOARD_LCD_MIRROR_X/Y`, `BOARD_LCD_SWAP_BYTES` in
  `board.h` let `MODE_TEST_PATTERN` correct any of this if wrong.
- **Flash mode** left at the IDF default (DIO), as in the Waveshare demo.
- **SPI clock 80 MHz** follows [ARD-DEMO]; [IDF-DEMO] uses 12 MHz which would
  cap the demo at ~13 fps. Fall back to 40 MHz (`BOARD_LCD_SPI_HZ`) if the
  image is corrupted.
- **Friction on the screen face** (`BACK_FRICTION`, Coulomb model) added so
  a flat board holds still regardless of sensor noise; it also gives a
  natural "starts sliding after a few degrees of tilt" feel.
- **Toolchain versions.** espressif32 platform 7.0.1 → ESP-IDF 6.0.1,
  `espressif/led_strip` ^3.0.0. A fixed `PROJECT_VER` is set in the root
  `CMakeLists.txt` because IDF's `git describe` fails in a repo with no
  commits.
- **Resting state** (speed < `REST_SPEED` while supported) added on top of the
  PixelDust-style integration; it is what makes the repose rule work, because
  without it gravity re-accelerates every grain every step.
- Sim memory is allocated by the app (internal RAM preferred, PSRAM fallback)
  and handed to `sim_init`, keeping `sim.c` free of ESP-IDF allocators.
