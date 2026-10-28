# Falling-Sand Phosphor Demo — Specification

Firmware for the **Waveshare ESP32-S3-LCD-1.47B**: a tilt-driven falling-sand
simulation rendered in the style of an old monochrome phosphor monitor.

This document is the source of truth for the design. It is kept in sync with
the code; deviations from the original brief are listed in §11.

---

## 1. Goals

- About one third of the screen is filled with glowing monochrome sand grains.
- Tilting the board makes sand flow, pile and slide; piles settle at a
  sand-like angle of repose (~30–35°), not flat like liquid.
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
| RGB LED | 1× WS2812B-0807 on GPIO38, byte order `BOARD_RGB_ORDER_RGB` (now RGB) | [SCH] `LED2 WS2812B-0807`, `RGB_IO`=IO38; [IDF-DEMO] `led_strip`, [ARD-DEMO] `neopixelWrite` (both GRB defaults, both only show a rainbow). Early bring-up reported pure red as red with GRB; 1.1.0 ledcal showed green 0x47FF00 (sent G90 R5) as red, i.e. the LED takes the first byte as red: switched to RGB. ledcal's start-up red/green/blue check settles it |
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
  sim.[ch]       grid + grains + step function, both resting rules, liquid levelling (NO ESP-IDF headers)
  material.[ch]  material table lookup                   (NO ESP-IDF headers)
  palette.[ch]   phosphor palettes + RGB565 LUT  (NO ESP-IDF headers)
  render.[ch]    glow/persistence buffer, frame composition (NO ESP-IDF headers)
  display.[ch]   ST7789 init, async DMA push, backlight PWM
  imu.[ch]       QMI8658 driver, axis mapping, low-pass, shake detection (own task)
  button.[ch]    BOOT button debounce, short/long press events
  led.[ch]       WS2812B status pulse
  idle.[ch]      idle dimming: activity detector + dim curve (NO ESP-IDF headers)
  main.c         app: modes; sand = sim task (core 0) + render task (core 1)
version.txt      firmware version (1.0.0), read by ESP-IDF
sdkconfig.defaults(.release)  base / release-only SDK settings
scripts/
  make_release.sh  build env:release, write dist/sand-physics-<version>.bin (merged image)
tools/
  sim_golden.c   PC test: CLASSIC reproduces Classic physics v1 bit for bit
  sim_host.c     PC harness: repose angles, tilt/lean/flat behaviour, shake recovery (classic)
  sim_liquid.c    PC harness: liquid levelling, material switching, tremor / wake
  sim_hand.c     PC harness: pool / pile held in hand (tremor, sway, lean wobble): awake + cost
  sim_liquidfx.c  PC harness: liquid drops on a pool, continuous pours, free fall, sloshing, knock, flip, heap slump
  sim_liquidstab.c PC harness: liquid comes to rest on its own (knock repro, 60 s random input)
  sim_sandfx.c    PC harness: sand (and gravel) stream arc and width, heap angles, rest after pour / shake / flip
  sim_idle.c      PC harness: idle dimming against the hand-motion model (hand_model.h)
  overrides.h     name=value material overrides shared by the host tools
  hand_model.h    hand-motion model shared by sim_hand and sim_idle
  sim_bench.c    PC benchmark: µs per step per material, seven scenarios
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
(`MATERIAL_CLASSIC_PARAMS`, `MATERIAL_SAND_PARAMS`, `MATERIAL_LIQUID_PARAMS`);
`material.c` holds the table. One engine; the material's *resting rule*
decides what a supported, slow grain does: `MAT_REST_REPOSE` (gravel, sand)
or `MAT_REST_SPREAD` (liquid), and its *flow model* how it moves:
`MAT_FLOW_CLASSIC` (gravel, v1) or the coherent path (sand, and always for
liquid; see "Sand" below).
`sim_set_material()` switches at any time: every grain stays where it is,
all are woken and the gravity-derived terms are recomputed.

| Field | CLASSIC (= v1) | LIQUID | Meaning |
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
| `spread_scan` / `spread_push` / `spread_step` | — | 24 / 0.4 / 0 | look-ahead, sideways push (cells/step), jump |
| `wave_gain` / `wave_damping` | — | 0.05 / 0.04 | wave flow gained per cell of imbalance per step / lost per step |
| `wave_window` / `wave_start` | — | whole tank / 4 cells | pressure window; imbalance that starts waves |
| `level_rate` | 0 (off) | 3 | wave flow cap per column pair (+ ¼ of the imbalance), grains/step |
| `level_global_rate` / `level_range` | — | 8 / 86 | quiet background levelling (only while waves are idle) |
| `level_max_motion` | — | 400 | levelling paused while total motion is above this (splashing) |
| `viscosity` | — | 0.15 | per step, a moving grain's velocity moves this far toward its neighbours' mean |
| `impact_absorb` | — | 0.8 | on landing, velocity pulled this far toward the neighbours' mean |
| `impact_to_flow` | — | 0 (off) | share of landing speed turned into sideways flow (never above the landing speed) |
| `knock_splash` / `knock_fan_deg` | — | 1.0 / 35 | launch speed per unit of shake strength (× 0.6–1.0); launch directions ± around the splash direction |
| `knock_depth` / `knock_max` / `knock_interior` | — | 1 / 40 / 0.1 | grains from each column top that can be thrown; most thrown per knock; sideways push on the rest |
| `wake_radius` | 0 (= `WAKE_RADIUS` 2) | 1 | cells around a vacated cell whose sleepers wake |
| `shade_spread` | 1.0 | 0.3 | palette shade variation |
| `name` (overlay / logs) | "gravel" | "liquid" | user-facing name; internally still `MATERIAL_CLASSIC` |

**CLASSIC verification** (host, x86-64 gcc -O2): `tools/sim_golden.c`
matches all 8 v1 state hashes bit for bit, and `tools/sim_host.c` output is
identical to v1 in every line (heap angles, lean, rotation, flat, tip, shake).
On the ESP32-S3 the compiler may fuse multiply-adds differently now that the
constants are struct fields, so the board can differ from v1 in the last
float bit, not in behaviour.

### Liquid model (2026-10-26, liquid round)

The liquid was called "water" until the liquid round (`MATERIAL_WATER`,
`step_water()`, `tools/sim_water*.c`); everything is now named liquid, and
the notes of earlier rounds below use the new name.

Liquid has its own step (`step_liquid`, `move_liquid`, `liquid_hit`) on the
same integration; gravel's code path is untouched (golden hashes match).

1. **Support and grounding.** A grain is supported by a wall or by a grain
   that is not falling away (moving away along gravity faster than 0.05
   cells/step). It is *grounded* if it is supported by a wall, a grounded
   grain or a sleeping one. **Only grounded grains rest.** When "down"
   changes (a flip, a tilt across 22.5°) the whole body is woken and
   `ground_sweep()` grounds, in every line along gravity, the run of grains
   packed against the wall; anything above a gap is ungrounded and falls. So
   a flipped pool falls as one body, while a resting pool stays at rest
   across a frame change.
2. **No sideways jumps in free fall.** The spread rule acts only on resting
   (hence grounded) grains. Before, any grain with a grain under it counted
   as supported, so grains on top of a falling body were treated as resting
   and pushed sideways (1137 pushes in the first 0.6 s of a flip; now 0).
3. **Falling bodies stay together.** A grain blocked by a grain moving the
   same way keeps pace with it; blocked by an ungrounded (still falling)
   grain it waits, keeping its speed but never more than one step of gravity
   faster than the grain ahead. Impacts happen only on walls and grounded
   grains. Awake liquid grains are updated farthest-along-gravity first
   (`liquid_sweep()`: a bucket sort on the line across gravity, random start
   and alternating direction within a line), so the grain ahead has always
   moved before the one behind it; with the old index order every follower
   updated before its leader lost a step and a falling body stretched into
   streams.
4. **Viscosity.** Each step, before gravity, a moving liquid grain's velocity
   moves `viscosity` of the way toward the mean velocity of the grains in
   its 4 neighbour cells (momentum sharing; resting neighbours count as
   still). The body flows freely but moves, falls and lands as sheets and
   blobs. There is no free-fall jitter or per-grain gravity any more.
5. **Landing is absorbed.** A landing grain (moving with gravity faster than
   `rest_speed` when blocked) has its velocity pulled `impact_absorb` (0.8)
   of the way toward the mean velocity of the grains around it, after a
   bounce of `elasticity` (0.05). It joins the local flow: a pour onto a
   pool, a slope or a moving wave stays calm where the stream meets the
   body. `impact_to_flow` can add sideways flow from the landing (capped at
   the larger of the grain's existing sideways speed and `impact_to_flow` ×
   the landing speed), but it is 0 by default. There is no impact splash:
   the fix round removed it (a pour set it off continuously, and grains
   thrown straight up their columns made vertical lines).
6. **Spray.** Grains thrown by a knock are marked spray (`LIQ_SPRAY` in
   `sim_t.ground`). When spray lands: no bounce, half its sideways speed
   kept, no flow, and it is never thrown again until it has rested (the mark
   clears when it is grounded and slow).
7. **Knock splash, the only splash.** Candidates are the top `knock_depth`
   (1) grains of each column, resting on something and not already spray.
   At most `knock_max` (100) per knock are thrown, picked at random over the
   whole surface; impulses less than 6 steps apart are one knock and share
   the cap (under sustained shaking it renews every 30 steps). Each thrown
   grain gets its own direction: the splash direction (up against gravity,
   leaning ~20° toward the knock) ± `knock_fan_deg` (35°), never within 8°
   of straight up its column, at `knock_splash` × strength × 0.6–1.0
   cells/step. So a knock throws a loose fan of droplets, not lines. The
   rest of the body gets `knock_interior` × the launch speed as a sideways
   push along the knock.
8. **Waves (sloshing).** A 1-D shallow-water model on the column heights
   replaces the long-range levelling transfers for real tilts. Each pair of
   neighbouring columns has a flow velocity `q` (grains/step) and an
   accumulator: `q += wave_gain × drive`, `q *= 1 − wave_damping`,
   `|q| ≤ level_rate + ¼|imbalance|`; whenever the accumulator passes a whole
   grain, the top grain of one column moves onto its neighbour. The drive is
   the difference between the mean heights of the pressure-carrying columns
   on either side (`wave_window`, default the whole tank, i.e. the
   fundamental sloshing mode) minus a one-cell dead band. Because `q` has
   inertia the surface overshoots level and rocks back. Waves start only
   above `wave_start` (4 cells, ≈ 5° across half the tank); smaller
   imbalances (hand sway) are finished by the quiet background levelling at
   `level_global_rate`. Dry columns carry no pressure, but liquid spills into
   a dry neighbour whose floor is lower in true gravity. Since the stability
   round the model always comes to rest:
   - Only columns with room above their top carry pressure (a column full to
     the grid edge can never level).
   - A column's top skips droplets, spray not yet at rest and grains still
     falling in (≥ `WAVE_FALLING` 0.6 cells/step along gravity) onto the body:
     landings only add mass where they settle. A line that is falling as a
     whole counts its first grain.
   - Only top grains slower than 0.5 cells/step are moved, and no move lifts
     a grain more than `WAVE_MAX_LIFT` (4) steps along gravity.
   - A pair that cannot move (no room, too steep) loses its flow; a pair
     whose top is still moving holds and loses a fifth per step.
   - Once the whole surface has been inside the dead band for 0.5 s the flow
     drains (×0.85/step); the waves end after 1 s there, or after 30 passes
     without a move.
   - Backstop: more than 2 s after the last input (shake, gravity wake,
     material, spawn), if the largest imbalance has reversed ≥ 3 times and
     the flow energy fell less than 30 % in the last second, or in any case
     once waves still run 4 s after the input, the drive is cut and the flow
     halved per step (`n_wave_backstop`).
9. **Spread rule** (grounded resting surface grains): as before, a sideways
   push (`spread_push` 0.4) toward a spot at least one cell lower in true
   gravity within `spread_scan`.
10. Glass friction and the frozen state work as for sand: lying flat, liquid
   holds still; it freezes at ~84° lean.
11. **Idle cost.** Small gravity changes wake only exposed liquid grains
    (frame changes wake all, item 1); submerged grains skip the spread
    look-ahead; levelling/waves are skipped while nothing can change and
    paused above `level_max_motion`; `wake_radius` 1.

### Material host results (`tools/sim_liquid.c`)

| Test | Result |
|---|---|
| Liquid spawn | settles in 4.4 s, surface within 1 cell |
| Tilt 0 → 45° | surface 40° at 0.75 s, 42° at 1.25 s, 43.7° (deviation 3.5 cells) from 4 s; at rest 5.9 s |
| Back 45° → 0° | level within 2 cells at 0.85 s, within 1 cell at 1.25 s, all asleep at 1.75 s |
| Classic heap (28-cell high) → liquid | within 2 cells at 0.35 s, 1 cell at 0.8 s |
| Liquid pool → classic | 0 grains move, 0 awake; next rotation of 50° piles at effective repose 31.3° (sand alone 31.0°) |
| Liquid lying flat, ±0.03 g noise | 0 grains moved |
| Held upright, ±0.03 g tremor (± 1° sway) | 0 of 4540 awake, both materials (v1 rule: 4540) |
| Deliberate 5° tilt during tremor | whole pile awake within 8 steps (67 ms), both materials |

At 45° (a diagonal gravity frame) the levelled surface sits within ~1.9
height units, which shows as 43.7° instead of 45° and 3.5 cells deviation
across the width; upright it levels to 1 cell.

### Liquid idle round (2026-10-26)

**Hardware before:** liquid held in hand ~7 ms/step mean, 8.5–9 ms max,
4540 awake with motion 0–11 for long stretches; gravel held 4 ms / 6–7 ms
(accepted as is); both 0 awake, 0.53 ms lying still. Render "fps 60 (40 new)",
sometimes 20–30 new, with the sim at 120 steps/s. One dip to 107 steps/s in a
hard liquid shake.

**Root cause, idle liquid.** `tools/sim_hand.c` (lean 20°, ±0.03 g tremor,
±3° + ±2° in-plane sway, ±2° lean wobble) reproduces it: the hand motion
crosses the 2° / 0.06 g wake limits about twice per second; every crossing
woke all 4540 grains, and they need 64 steps to fall asleep again, so on
average ~3250 were awake (gravel the same: accepted). For liquid each awake
resting grain also ran the spread look-ahead every step (rule_prob 1.0) and
the levelling pass rescanned the grid every step. Levelling transfers
(50–80/s following the sway) were a small part; spread pushes were 0.

**Fix:** the "Idle cost" item of the liquid model above. Hand-held host results:

| | before | after |
|---|---|---|
| liquid, held: awake (20 s mean) | 3254 | 73 |
| liquid, held: host µs/step mean | 90.5 | 12.6 |
| gravel, held (unchanged) | 3240 awake, 64 µs | same |
| liquid, 5° tilt during tremor | — | first grains move after 4 steps (33 ms); surface only wakes (peak 160) |

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

**Liquid cost** (host µs/step mean / worst; `tools/sim_bench.c`):

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
liquid), moving work ~14× (gravel shake worst 6–7 ms ↔ 461 µs). Liquid held in
hand: ~0.9 ms/step (was ~7). All-awake bursts: ~3.5 ms (was ~8.6). Hard
shake: mean ~3.4 ms, single worst ~6.8 ms, the same as gravel.

**Liquid look**, previous round → now (`tools/sim_liquid.c`): 45° tilt surface
40.1° at 0.75 s in both, 43.7°/3.5 cells final in both, at rest 5.9 s → 5.0 s;
back to upright level in 0.85 s in both; heap → liquid within 2 cells at 0.35 s
in both, final 1 → 2 cells; liquid → gravel then 50°: effective repose 31.3° →
31.1°; lying flat 0 moved in both (145 → 2 awake); freeze point ~84° in both.

### Liquid behaviour round (2026-10-26)

`tools/sim_liquidfx.c` (new) measures the behaviours; the "previous" column is
the previous round's liquid (same build with the liquid step routed back
through the gravel step and generic shake).

| Test | previous round | now |
|---|---|---|
| Flip: spread pushes in the first 0.6 s | 1137 | 0 |
| Flip: falling grains' sideways speed, max | 0.81 cells/step | 0.30 |
| Flip: grains falling at 0.1 / 0.2 s | body peeled from its leading edge | 96 % / 99 % |
| Flip: body during the fall (53 rows deep) | — | 58 → 73 → 120 → 156 rows tall at 0.1/0.2/0.4/0.6 s, mostly sparse rows (streams) |
| Flip: impact droplets | — | up to 72 grains thrown back, 20th highest 7 cells |
| Flip: level after landing | 4.85 s | 3.05 s |
| Tilt 10° and back | no overshoot, quiet re-level in 0.17 s | +8.3° → −3.5° (visible overshoot), settled 4.4 s |
| Tilt 25° and back | no overshoot, 0.42 s | 1–3 swings past level depending on the run (e.g. +15.8° → −1.8° → +1.3° → −2.6°), settled 4–5 s |
| Knock (1 g) | droplets 4 cells high, 2 grains airborne, down in 0.17 s | droplets up to 35 cells, 41 airborne, down in 0.9 s |
| Gravel heap → liquid | within 2 cells in 0.35 s (teleport levelling) | spread to ±10 cells in ~0.7 s, within 2 cells in 3.1 s (wave ripples decay) |
| 45° tilt and back (old suite) | at rest after 5.0 s | at rest after 6.7 s (sloshing and splash) |
| Lying flat ±0.03 g noise; liquid → gravel | 0 moved; piles at 31.1° | unchanged (0; 31.2°) |
| Held in hand (`tools/sim_hand.c`) | 73 awake, 12.6 µs | 73 awake, 14–19 µs (host noise between runs) |

**Cost** (`tools/sim_bench.c`, host µs/step mean / worst, same run for both
materials; previous-round liquid in brackets, from its own run):

| Scenario | gravel | liquid now | liquid previous round |
|---|---|---|---|
| awake (all woken, resting) | 93.8 / 198.7 | 77.6 / 192.6 | (50.1 / 124.7) |
| shake | 308.6 / 549.5 | 367.0 / 640.9 | (241.9 / 483.9) |
| freefall | 239.2 / 311.2 | 326.1 / 473.0 | (248.1 / 333.5) |
| steer (1°/step) | 159.0 / 344.6 | 311.7 / 677.8 | (189.9 / 388.7) |
| slosh | 100.3 / 150.1 | 155.5 / 352.9 | (96.4 / 204.8) |

Liquid now moves more (mean speed in a shake 1.57 vs gravel 0.44 cells/step,
1.8 vs 1.1 sub-steps per moving grain): that is the requested behaviour, and
most of the extra cost. Board estimate (≈ 14× host for moving work): liquid
shake mean ~5 ms, single-step worst ~8–9 ms; continuous steering mean
~4.4 ms, worst ~9 ms; held in hand ~1–1.3 ms; all-awake bursts ~5 ms. The
worst single steps may exceed the 7 ms target during sustained hard shaking
or continuous rotation; see the report for options.

### Liquid round (2026-10-26)

**Hardware before:** the water was jumpy, as if constantly agitated; its own
droplets landing on the body set off more splashes until the surface erupted.
Wanted: a slightly viscous liquid that still splashes, and no feedback.
Changes: the rename (overlay "LIQUID", looks "green liquid" / "orange
liquid"), items 3–8 of the liquid model as they were then (update order, viscosity,
capped impact flow, spray, collective energy-bounded impact splash, no
free-fall jitter) and `wave_damping` 0.01 → 0.04.

`tools/sim_liquidfx.c` results (defaults):

| Test | result |
|---|---|
| 5 drops from 20 cells onto a settled pool | land at 0.21 s; after that total motion rises on 1 step by 0.02 (one grain picking up speed down a slope), never over a 5-step sum; 0 splashes; pool at rest at 0.53 s |
| 5 drops, 40–68 cells (staggered) | rises on 5 single steps (largest 0.08), never over 5-step sums; 0 splashes; at rest 0.53 s |
| 20 drops, 10–67 cells | 1 single-step rise (0.03); 0 splashes; at rest 0.53 s |
| Knock (1 g, 4 impulses) | droplets up to 14 cells above the surface, 54 grains in the air at most, all down after 0.40 s, all spray absorbed and at rest after 0.39 s; **1** knock event, **0** impact splashes after it, 1175 spray landings absorbed |
| Flip (53 deep, falls 107 cells) | 100 % falling at 0.1 s; body 58 → 65 → 75 → 73 rows tall at 0.1/0.2/0.4/0.6 s, at most 49 loose grains (no neighbour) before landing (was 1213 with the old update order); lands 0.59 s |
| Flip: impact splash | 1 burst (13 trigger steps), 203 grains thrown as spray, up to 31 in the air at once, 5th-highest 10 cells above the level surface; 0 secondary splashes; level within 2 cells at 1.25 s |
| Splash energy | thrown / landed (Σ v²) 0.27 at most, bound 0.30 |
| Tilt 10° and back | +10.5° → −2.2°, 1 swing past level, settled 1.51 s |
| Tilt 25° and back | +15.3° → −1.7°, 1 swing past level, settled 1.54 s |
| Flip: spread pushes / falling grains' sideways speed | 0 / mean 0.01, max 0.83 cells/step (a few edge grains) |
| Gravel heap → liquid | within 2 cells of level at 1.40 s |

Held in hand (`tools/sim_hand.c`): liquid 73 awake, 13.5 µs/step on the host
(unchanged); gravel unchanged.

**Cost** (`tools/sim_bench.c`, host µs/step mean / worst):

| Scenario | gravel | liquid | liquid, `viscosity=0` |
|---|---|---|---|
| awake (all woken, resting) | 97.2 / 179.7 | 91.7 / 216.9 | 90.9 / 203.5 |
| shake | 311.0 / 490.5 | 389.6 / 629.0 | 388.3 / 660.3 |
| freefall | 235.1 / 338.6 | 345.7 / 527.7 | 294.4 / 436.0 |
| steer (1°/step) | 164.4 / 333.2 | 329.2 / 629.8 | 293.6 / 596.5 |
| slosh | 103.0 / 197.0 | 174.0 / 394.3 | 166.5 / 325.3 |

Board estimate (≈ 14× host for moving work, ≈ 65–70× for awake-resting):
liquid shake mean ~5.5 ms, worst ~9 ms; steering mean ~4.6 ms, worst ~9 ms;
held in hand ~1 ms. As in the previous round, single worst steps may pass
the 7 ms target during sustained hard shaking or continuous rotation.

Simulation memory grew by ~15 KB (update order, impact and pending-splash
lists) to ~138 KB.

### Liquid fix round (2026-10-26)

**Hardware before:** the liquid poured well, but splashes made tall vertical
lines of grains, a pour set off exaggerated reactions wherever the stream met
the body (feedback), and splashes moved too much material. Causes: the impact
splash triggered on a global count of fast landings per step, so a steady
pour kept triggering it, and it threw grains straight up their columns.

**Changes:** impact splash removed; landings absorbed into the local flow
(`impact_absorb` 0.8, `impact_to_flow` 0); the knock is the only splash,
surface-only, capped and fanned (items 5–7 above). Viscosity, update order,
waves and idle handling unchanged.

`tools/sim_liquidfx.c` results (defaults), previous round in brackets:

| Test | result |
|---|---|
| Pour, 2-wide stream for 4 s onto a settled pool | 1871 landings absorbed; **0** cells: no grain near the impact rises above the local surface; mound beside the stream ≤ 1 cell; fastest sideways grain above the surface 0.40 cells/step; **0** splash events |
| Pour onto the high side of a returning 25° wave | 3357 landings absorbed; **0** cells risen; height beside the stream ≤ 9 cells above the window median (the wave alone, no pour: 13); 0.41 cells/step sideways; **0** splash events |
| Same metric with absorption off and bounce on (`impact_absorb=0 impact_to_flow=0.6 elasticity=0.5`), to check it detects reactions | grains rise 6 cells (pool) / 19 cells (wave) |
| Knock (1 g, 4 impulses) | 100 grains thrown (cap); launch angles −15°…+53° from up (mean +21°, sd 21°; none within 5° of vertical); 0.54–1.43 cells/step; up to 10 cells high, 53 in the air at most, longest vertical run of airborne grains 2; all down 0.28 s, all absorbed and at rest 0.31 s; **1** splash event [3396 grains moved as spray, 14 cells, 54 airborne] |
| Drops onto a pool (5 @ 20, 5 @ 40–68, 20 @ 10–67 cells) | 0 splash events; motion never rises over 5-step sums (per step: one rise of 0.06 in the first case, none in the others); at rest 0.53 s in all |
| Tilt 25° and back | +15.7° → −1.5°, 1 swing, settled 1.55 s [+15.3° → −1.7°, 1 swing, 1.54 s] |
| Tilt 10° and back | +10.7° → −2.0°, 1 swing, settled 1.60 s [+10.5° → −2.2°, 1.51 s] |
| Flip | body 58 → 66 → 77 → 79 rows tall at 0.1/0.2/0.4/0.6 s, ≤ 49 loose grains; lands 0.59 s; 0 grains rise more than 2 cells above the level surface; 0 splash events; level at 1.10 s [same fall; 203 grains thrown, 31 airborne, 10 cells; level 1.25 s] |
| Free fall: spread pushes / sideways speed of falling grains | 0 / mean 0.01, max 0.06 cells/step [0 / 0.83 max] |
| Gravel heap → liquid | within 2 cells at 1.30 s [1.40 s] |

Held in hand (`tools/sim_hand.c`): liquid 73 awake, 13.5 µs/step host
(unchanged); gravel unchanged.

**Cost** (`tools/sim_bench.c`, host µs/step mean / worst; previous round in
brackets):

| Scenario | gravel | liquid |
|---|---|---|
| awake | 104.4 / 186.9 | 92.5 / 297.9 [91.7 / 216.9] |
| shake (every other step) | 316.1 / 511.3 | 138.3 / 293.2 [389.6 / 629.0]; 297 grains moving instead of 4289 |
| freefall | 242.2 / 342.0 | 360.1 / 492.0 [345.7 / 527.7] |
| steer (1°/step) | 166.5 / 341.5 | 337.7 / 685.1 [329.2 / 629.8] |
| slosh | 108.5 / 229.4 | 175.4 / 302.7 [174.0 / 394.3] |

Board estimate (≈ 14× host for moving work): liquid shake mean ~2 ms, worst
~4 ms; continuous steering mean ~4.7 ms, worst ~9.6 ms (single steps may
pass the 7 ms target); held in hand ~1 ms. Simulation memory ~132 KB (the
impact lists are gone).

### Liquid stability round (2026-10-26)

**Hardware before:** pours and a knock on a calm body looked right, but when
splashed grains fell back the body over-reacted, and sometimes the surface
columns kept bobbing like a spectrum display with no input at all. Splashes
were also too big.

**Root cause (confirmed on the host, `tools/sim_liquidstab.c`).** The wave
model was pumping: its moves lifted grains, gravity dropped them back, and
nothing stopped the next lift. Three faults made that possible, each found by
summing the height change of every wave transfer against that of the rest of
the physics (an instrumented host copy of `sim.c`):
1. *Unreachable drive.* In a diagonal gravity frame (board held between
   22.5° and 67.5°) some "columns" are lines that start at a side wall and are
   full. They can never take a grain, but their heights were part of the
   whole-tank pressure means, so the imbalance could never be levelled. Pairs
   kept a steady flow and lifted grains up to 8.5 cells onto their
   neighbours (repro: held at 30°, flow energy constant at ~124, ~550
   transfers/s for 40 s).
2. *Unbounded lifts.* A move puts the top grain of one column on the
   neighbouring column's top, wherever that is. In disturbed bodies single
   moves lifted grains up to ~70 cells onto stray tops far from the body;
   in the leaned-back random runs the waves lifted +21 000…+48 000 units of
   height per second and gravity dropped the same back.
3. *Double-moved mass.* The waves also moved top grains that were moving at
   up to 1 cell/step, i.e. mass the particle body was already carrying, each
   with its own inertia. Held at 25° the surface swung +9° → −12.5° → +10.9°
   → −16.1°, growing. And a pour touching the body is one unbroken line of
   grains, which read as a towering column: the waves drove mass away from
   it all the time and dug a pit under the stream (33 cells deep on a
   settled pool) — the exaggerated reaction where a stream meets the body.
Spray landing only started it: a knock or the landing disturbed the surface
enough to start the waves, which then never stopped.

**Changes** (all in `wave_pass()` / `column_scan()`; item 8 above):
- Full columns carry no pressure (`wave_wet()`).
- No move lifts a grain more than `WAVE_MAX_LIFT` (4) steps along gravity.
- Only top grains slower than 0.5 cells/step are moved (`WAVE_SRC_SPEED2`).
- Column tops skip droplets, spray not yet at rest and grains still falling
  in faster than `WAVE_FALLING` (0.6 cells/step) onto a body: landings only
  add mass where they settle.
- Dead band (≤ 1 cell drives nothing); once the whole surface has stayed in
  it 0.5 s the flow is drained (×0.85/step), and after 1 s the waves end.
  A pair that can't move loses its flow; waves end after 30 passes without a
  move.
- Backstop: the drive is cut and the flow halved per step if, more than 2 s
  after the last input, the largest imbalance has reversed ≥ 3 times and the
  flow energy has not fallen 30 % in the last second (self-sustained), or
  in any case once waves are still running 4 s after the last input.
- Splashes: `knock_max` 100 → 40, `knock_splash` 1.6 → 1.0.

**Results** (host; "before" = the previous round's wave model and splash
settings, rebuilt in `build-host/` for comparison):

| Test | before | after |
|---|---|---|
| Repro: settled pool, 5 knocks 0.25 s apart, 40 s no input — upright / leaned 20° | rest 1.15 s / 1.07 s | rest 0.48 s / 0.51 s |
| … held at 30° / at 30° leaned 20° | **never** (flow energy ~124 constant, ~550 transfers/s for 40 s) | rest 0.44 s / 0.46 s (flow energy 0 from 2 s) |
| … held at −120°, leaned 20° | rest 1.00 s | rest 0.52 s |
| Random: 20 × (60 s random knocks, tilts, pours; 10 s no input) | 12 of 20 at rest | 19 of 20 at rest (slowest 5.4 s); run 18 still creeping at 10 s (spread rule, leaned back, diagonal frame upside down; no wave activity; at rest at ~22 s) |
| Backstop | — | 0 in every repro, tilt, flip, pour-on-pool and knock test; 5 times in the 20 random runs (always the 4 s rule after a big rotation into an odd pose); 3 in "pour onto a returning wave" (host pours give the sim no input, a real pour comes from a tilt) |
| Knock (1 g, 4 impulses) | 100 grains, droplets 10 cells, 53 in the air, absorbed 0.31 s | 40 grains, 3 cells, 18 in the air, absorbed 0.18 s; 1 splash event |
| Pour on a settled pool | beside the stream +1 / **−33** cells (pit) | +5 / −1 cells; 0 grains rise |
| Pour on a returning 25° wave (wave alone: +6 / −9) | +9 / −44 | +14 / −12 |
| Tilt 10° and back | +10.7° → −1.9°, 1 swing, 1.60 s | +9.4° → −1.5°, 1 swing, 1.49 s |
| Tilt 25° and back | held reaches 16.3°; +15.7° → −1.4°, 1 swing, 1.55 s | held reaches 18.4°; +17.7° → level, no swing, 2.32 s |
| Flip | ≤ 49 loose grains; level 1.10 s | ≤ 55 loose grains; level 0.85 s |
| Drops on a pool | 0 splashes, rest 0.53 s | same |
| Gravel heap → liquid | 1.30 s | 1.60 s |

(The tilt tests now average the surface angle over 50 ms: the single-step fit
jumps by several degrees while the body moves, which made earlier swing
counts partly noise; the previous-round numbers above are re-measured with
it.)

Held in hand: liquid 73 awake, 14.5 µs/step host (unchanged). Cost
(`tools/sim_bench.c`, liquid, host µs mean / worst): shake 121 / 267, steer
332 / 684, freefall 343 / 449, slosh 160 / 333, awake 93 / 234. Board
estimate (×14): steering ~4.6 ms mean, ~9.6 ms worst single step; shake
~1.7 / ~3.7 ms; held ~1 ms. Simulation memory ~132 KB.

### Sand (hourglass round, 2026-10-26)

A third material, `MATERIAL_SAND` ("sand", overlay "SAND"): open-hourglass
sand. It piles like gravel — the same angle-of-repose rule, repose 33°, scan,
dither, glass friction and lean behaviour — but moves on the liquid's
coherent path (`flow_model = MAT_FLOW_COHERENT`; `step_liquid()` /
`move_liquid()` with the liquid-only parts zero in its parameters):
- **Coherent fall.** A grain blocked by a grain moving the same way keeps
  pace with it instead of bouncing off; awake grains are updated farthest
  along gravity first; only grains grounded through a still chain to a wall
  rest. A falling stream stays 2–4 columns wide (gravel: ~12).
- **Momentum.** Velocity is integrated as for gravel, with less drag
  (`damping` 0.002) and a higher cap (`max_speed` 4 cells/step). Moves are
  sub-stepped at most one cell per axis per sub-step (n = ⌈max(|vx|,|vy|)⌉
  ≤ 4 ≤ `SIM_MAX_SUBSTEPS` 8), so no cell is skipped. Turning the board bends
  a falling stream gradually.
- **Bounce.** `elasticity` 0.15 (gravel 0.30). Nothing adds energy on
  landing: no splash, impact flow or absorption; the knock is the generic
  shake.
- **Cheap in the hand.** Small gravity changes wake only exposed grains (a
  frame change wakes all and re-grounds, as for liquid); buried resting
  grains skip the repose look-ahead.
- The renderer's speed table now spans a fixed 2 cells/step (the glow
  saturates at 0.75), so the faster sand does not change how gravel and
  liquid look; `material_max_speed()` is gone.

| Parameter | sand | (gravel) |
|---|---|---|
| `gravity_scale` / `damping` / `elasticity` | 0.06 / 0.002 / 0.15 | 0.06 / 0.01 / 0.30 |
| `max_speed` / `jitter` | 4.0 / 0.005 | 2.0 / 0.01 |
| `contact_friction` / `rest_speed` / `glass_friction` | 0.30 / 0.15 / 0.15 | same |
| `rule_prob` / repose angle / scan / diag correction / dither | 0.35 / 33° / 4 / 4° / 0.15 | same |
| `wake_radius` | 0 (= 2) | same |
| `flow_model` | `MAT_FLOW_COHERENT` | `MAT_FLOW_CLASSIC` |
| `shade_spread` | 0.6 | 1.0 |

Host results (`tools/sim_sandfx.c`; gravel alongside):

| Test | gravel | sand |
|---|---|---|
| Stream, gravity turned 45° mid-fall: mean direction of the grains in the air (0 ms / 50 / 100 / 200 / 300 / 400 ms) | 0° / 8° / 16° / 29° / 39° / 42° at ~1.7 cells/step | 0° / 5.5° / 10° / 19° / 30° / 35° at 2.4–3.4 cells/step (about 1° per step at first) |
| Falling stream width (2-wide source, rows 30–89) | 11.9 mean / 14 max columns | 2.8 / 4 |
| Heap angle, lean 0 / 30 / 45 / 60 / 75° (3 seeds) | 33.2 / 34.7 / 37.3 / 43.7 / 52.5° | 33.2 / 34.6 / 37.4 / 43.7 / 52.5° |
| Full rest (all asleep) after a 2 s pour / a 0.25 s shake / a flip | 2.76 / 1.69 / 5.82 s | 2.28 / 2.61 / 1.65 s |
| Mechanical energy (kinetic + potential) rising on its own | never | never over any 0.5 s window; single steps up to +63 in the first second of a flip (a grain waiting behind the grain ahead keeps its speed for one step without moving, paid back on the next; the same follower rule as liquid) and ≤ 0.004 from repose creep tokens |
| Held in hand (`tools/sim_hand.c`) | 3240 awake, 75 µs/step | 42 awake, 7.2 µs/step |

Cost (`tools/sim_bench.c`, host µs mean / worst; ×14 board for moving work,
×65–70 for awake-resting): sand shake 362 / 577 (board ~5.1 / ~8.1 ms), steer
262 / 547 (~3.7 / ~7.7 ms), freefall 319 / 488, awake 92 / 261 (~6.0–6.4 ms
mean); gravel shake 308 / 497 (~4.3 / ~7.0 ms). The mean is inside the 7 ms
target; single worst steps during sustained hard shaking may reach ~8 ms
(gravel ~7, liquid steering ~9.6). Neither a lower speed cap (2.5–4: no
change) nor dropping the jitter (no change, and the stream turns into a
perfectly straight 2-column line) helps: the extra cost is the per-grain
coherent collision handling that keeps the stream together. Skipping the
repose look-ahead for buried grains cut the awake case from 132 to 92 µs.

## Release 1.0.0 (2026-10-26)

No physics or material changes: gravel's golden fingerprints, the liquid
suite (`sim_liquidfx`, `sim_liquidstab`, `sim_liquid`) and the sand suite
(`sim_sandfx`) produce output identical to the previous round.

- **Version.** `version.txt` (1.0.0) is the only place: ESP-IDF reads it when
  `PROJECT_VER` is unset (the old fixed `set(PROJECT_VER "0.1.0")` is gone),
  it ends up in the app descriptor (`esp_app_get_description()->version`)
  and the boot banner. No git needed, so clean checkouts and archives build
  the same version.
- **`env:release`.** The demo with `-DRELEASE_BUILD=1`, also passed to CMake
  (`board_build.cmake_extra_args`), where `CMakeLists.txt` adds
  `sdkconfig.defaults.release`: `CONFIG_LOG_DEFAULT_LEVEL_WARN`,
  `CONFIG_BOOTLOADER_LOG_LEVEL_WARN`, and `CONFIG_LOG_MAXIMUM_LEVEL_INFO` so the
  app's tag can be raised again at run time. `STATS_LOG` (config.h) defaults
  to 0 under `RELEASE_BUILD`, else 1; `-DSTATS_LOG=1` brings the statistics
  line back (the app then raises its own tag to info). The banner uses
  `printf` and always prints, with a `build : release / development` line.
- **Idle dimming** (`src/idle.c`, hardware-free). The sim task feeds every
  step's filtered gravity, shake flag and sim motion to `idle_update()`;
  activity is a shake, gravity more than `IDLE_WAKE_TILT_DEG` (15°) from its
  direction at the last activity, motion above `IDLE_WAKE_MOTION` (20), or a
  button press (render task). After `IDLE_DIM_AFTER_S` (180 s) without
  activity, `idle_dim_amount()` ramps 0 → 1 over `IDLE_DIM_FADE_MS` (2 s):
  backlight `BACKLIGHT_PERCENT` (90 %) → `IDLE_DIM_BACKLIGHT_PERCENT` (8 %),
  LED colour × 1 → `IDLE_DIM_LED_SCALE` (0.2). Activity resets it on the next
  frame. `IDLE_DIM_ENABLE 0` turns it off. `tools/sim_idle.c` (hand-motion
  model shared with `sim_hand` in `tools/hand_model.h`): held in the hand for
  10 min, the board moves at most 5.8° and sim motion stays ≤ 0.2 for all
  three materials, so there are 0 activity events and dimming starts at
  180.0 s; a 30° tilt while dimmed counts after 167 ms, a knock at once.
- **Merged image.** `scripts/make_release.sh` builds `env:release` and runs
  `esptool merge_bin` with the offsets and flash settings from the build's
  `flasher_args.json` (0x0 bootloader, 0x8000 partition table, 0x10000 app;
  DIO, 80 MHz, 16 MB) into `dist/sand-physics-<version>.bin`, flashed at 0x0.

## Power 1.1.0 (2026-10-28)

No physics or material changes: every host suite (`sim_golden`, `sim_host`,
`sim_hand`, `sim_liquid`, `sim_liquidfx`, `sim_liquidstab`, `sim_sandfx`,
`sim_idle`) gives output identical to 1.0.0 apart from host timing columns.

- **Hardware checked against the schematic** (`[SCH]`): QMI8658 INT1 (pin 4)
  → `IMU_INT1` → IO13, INT2 (pin 9) → `IMU_INT2` → IO12; BOOT (Key2) to GND
  with R10 10K pull-up on IO0; RESET (Key1) on CHIP_PU. The LED data net
  `RGB_IO` has a 10K pull-up R21 to 3V3; the LED's VDD is 3V3, unswitched.
  Power path: BAT → AO3401 → VSYS (VBUS → MBR230LSF → VSYS) → ME6217C33
  (EN tied to VSYS) → 3V3; divider R1 200K / R4 100K from BAT to IO1.
  Backlight: GPIO46 → 1K → SI2302 gate with 10K pull-down. Flash and PSRAM
  on VDD_SPI. Neither Waveshare demo uses the IMU interrupts.
- **Gestures** (`src/gesture.c`, hardware-free, debouncing included): click
  (fires `BTN_DOUBLE_MS` after release), double click, long press (fires on
  release between `BTN_LONG_MS` and `BTN_HOLD_SHOW_MS`), hold (bar from
  `BTN_HOLD_SHOW_MS`, release cancels, `BTN_POWER_OFF_MS` powers off while
  held). A press down at boot is ignored until released. `tools/sim_power.c`
  checks 19 scripted sequences with random contact bounce and sweeps press
  length and gap.
- **Power off** (`src/power.c`): CRT collapse (`render_crt()`; squeeze 300
  ms, line → dot 200 ms, fade 200 ms), then `led_sleep()` (RMT released, data
  held high: no current against R21, nothing latched), `display_sleep()`
  (DISPOFF, SLPIN, bus and control pins held), QMI8658 wake-on-motion
  (datasheet §9.4: CTRL7 = 0, CTRL2 = ±4 g / 11 Hz low-power, CAL1_L =
  threshold mg, CAL1_H = INT2 idle-high + blanking, CTRL9 0x08, CTRL1 INT2
  enable, CTRL7 = accel only), wait for BOOT up, deep sleep with ext1
  (ANY_LOW) on GPIO0 and a `WOM_ARM_DELAY_S` timer. The timer wake (no
  display, no LED) resets INT2 and sleeps again with GPIO0 + GPIO12. Mode
  saved in `RTC_DATA_ATTR` memory (magic-checked; used only after a
  deep-sleep wake). `imu_init()` leaves wake-on-motion (datasheet §9.6) and
  soft-resets the sensor on every boot.
- **GPIO0 wake.** Strapping pins are latched at system / chip reset only;
  a deep-sleep wake is a core reset (TRM table 7-1), so a BOOT press is a
  safe wake source. The ext1 pins are returned from the RTC mux at boot.
- **Auto-off**: `idle_auto_off()` on the idle-dimming activity time.
- **LED colour**: per-palette `LED_COLOR_*` (plain 0xRRGGBB), maths in
  `src/ledcolor.c`: adjustments in gamma-encoded HSV, then `LED_GAMMA` once
  per channel, then brightness (pulse, dimming, animation) scales the linear
  channels together. Channel order is only the driver's
  `color_component_format`, from `BOARD_RGB_ORDER_RGB`. `env:ledcal`
  (`MODE_LED_CAL`) starts with a 1 s red / green / blue check, then steps
  candidates.

## Battery (2026-10-28)

No physics changes; every host suite identical (timing columns aside).

- **Hardware checked against `[SCH]`**: BAT → R1 200K 1 % → `BAT_ADC` → R4
  100K 1 % → GND, C8 100nF on BAT_ADC; GPIO table "IO1 BAT_ADC"; `[IDF-DEMO]`
  `BAT_Driver.h`: ADC1 channel 0, × 3, / 0.992857. The divider is on BAT,
  before the AO3401, so it reads the cell (or the charger output, with no
  cell) on USB too. ETA6098 STAT → LED1 (via R3 3K) only; VBUS → D1, the
  AO3401 gate network (R7 1K / R6 100K) and the header, no GPIO.
- `src/battery.c`: ADC oneshot, 6 dB, curve-fitting calibration, 16
  conversions per reading, task priority 1 on core 1, once per second.
- `src/batt.c` (hardware-free, `tools/sim_battery.c`): EMA (60 s, started as
  a running mean), LiPo curve (5 % table, 3.27–4.20 V), shown percent
  follows down at −2 %, up at +10 % (2 % while settling), 5 bars with 2 %
  boundary hysteresis, low flag below 15 % / off at 17 %, charging hint from
  a 5 s EMA above 4190 mV (off below 4160 mV). Under ±40 mV noise the shown
  level changes at most twice in 10 min at any fixed voltage and never rises
  during a 3 h discharge.
- OSD (`render_battery()`): vertical icon 64 × 128 px with nub, 5 bars,
  1-px halo, unlit slots at 8 %, black backing box; 3 s, last 0.4 s fading;
  charging: 0..5 bars cycling every 0.3 s. Low battery: LED red 150 ms every
  1 s instead of the pulse.

## 1.2.0: cutoff and drain log (2026-10-28)

- **Cutoff** (`BAT_CUTOFF_*`): running, the 60 s filtered reading (after 10
  readings) below 3300 mV and not on USB → empty icon blinking 2 s + LED red,
  CRT collapse, `power_off(POWER_OFF_CUTOFF)`: QMI8658 to power-down (no
  wake-on-motion, no arming timer), ext1 on BOOT only. Every boot in the sand
  modes checks the settled boot reading (taken before the backlight is on)
  against 3300 mV, or 3400 mV after a cutoff; below: warning over black,
  line/dot collapse, sleep again. "On USB" = charging hint (> 4190 mV) or
  `usb_serial_jtag_is_connected()`, checked after the warning so a host has
  time to enumerate.
- **Drain log**: `power_off()` stores `battery_read_settled_mv()` (8
  conversions discarded, 20 ms, 64 averaged; after the backlight is off) and
  `gettimeofday()` in RTC memory with the reason. A wake from that sleep
  shows sleep time, both voltages and mV/day in the banner (`last off`).
  RTC clock: internal 136 kHz RC (no 32 kHz crystal on the board), a few %.
- LED colours `0x2DFF00` / `0xFF9C00` hand-matched on hardware with ledcal.

## Mode-name overlay

On a BOOT long press, and once at boot, the material name of the new look
("GRAVEL", "SAND", "LIQUID") is drawn centered in the palette's brightest shade
(`palette_brightest()`) in a built-in 5×7 terminal font scaled ×4
(`OVERLAY_SCALE`, reduced automatically if it would not fit; "GRAVEL" is
140 px wide), over a backing box `OVERLAY_PAD` (6) px larger than the text.
Full strength for `OVERLAY_HOLD_MS` (600 ms), then box and text fade out
together over `OVERLAY_FADE_MS` (400 ms): the box darkens the grains behind it
by alpha and the text is added on top. Rendering only (core 1, after
`render_frame()`, ~6 k pixels); the simulation keeps running. Looks are
named "green gravel", "orange gravel", "green sand", "orange sand", "green
liquid", "orange liquid".

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
  `shade_spread` scales the variation (classic 1.0; liquid 0.3 for a smoother
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
  table: green gravel → orange gravel → green sand → orange sand → green
  liquid → orange liquid → back to the start. BOOT long press steps through it; every grain stays where it
  is, only colour and behaviour change (a settled sand pile slumps flat as
  liquid; liquid switched to sand rests and piles on the next tilt). The mode
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
| BOOT click (< `BTN_LONG_MS`, no second press within `BTN_DOUBLE_MS`) | respawn sand scattered in the top part of the screen |
| BOOT double click | battery display for `BAT_OSD_MS`; prints the reading |
| BOOT long press (released between `BTN_LONG_MS` and `BTN_HOLD_SHOW_MS`) | next sand-mode look: green gravel → orange gravel → green sand → orange sand → green liquid → orange liquid |
| BOOT hold (`BTN_POWER_OFF_MS`) | power off (deep sleep); released while the `OFF` bar shows: cancelled |

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

| Scenario | v1 classic | classic now | liquid |
|---|---|---|---|
| settled | 6.9 / 28.5 | 6.7 / 32.2 | 7.0 / 25.5 |
| freefall (3580–3730 moving) | 242.8 / 337.0 | 206.6 / 308.8 | 240.2 / 361.7 |
| shake (worst case) | 322.3 / 541.1 | 257.3 / 460.6 | 249.2 / 482.5 |
| flow (turn 90°) | 37.9 / 145.2 | 35.9 / 123.7 | 72.6 / 219.5 |
| slosh (±45° every 0.5 s) | 91.6 / 194.9 | 87.4 / 177.6 | 147.0 / 302.8 |
| steer (1°/step, nothing sleeps) | 157.5 / 356.9 | 142.9 / 383.8 | 208.1 / 424.5 |

**Board estimate:** the hardware figures for v1 (4–6 ms mean, ~9 ms max in
heavy motion) are ~17× the host v1 shake numbers. On that ratio the worst
step becomes ~7.8 ms classic and ~8.2 ms liquid before the IRAM and IMU-core
changes, which the host cannot measure; they remove flash-cache misses
shared with the render core and 200 Hz preemption of the sim task. Expected
worst case therefore ~6.5–7.5 ms for both materials — at or slightly above
the 7 ms target. Held in hand, settled: ~0.1–0.2 ms (was 4.4 ms).

If the board still shows worst steps above ~7 ms, the remaining levers all
change behaviour, so they are left for a decision: fewer grains
(`FILL_FRACTION`), a lower liquid `max_speed` or `level_rate`, or capping
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
