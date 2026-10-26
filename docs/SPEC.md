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
  config.h       every tunable: app mode, palette, grain size, physics, look
  sim.[ch]       grid + grains + step function   (NO ESP-IDF headers)
  palette.[ch]   phosphor palettes + RGB565 LUT  (NO ESP-IDF headers)
  render.[ch]    glow/persistence buffer, frame composition (NO ESP-IDF headers)
  display.[ch]   ST7789 init, async DMA push, backlight PWM
  imu.[ch]       QMI8658 driver, axis mapping, low-pass, shake detection (own task)
  button.[ch]    BOOT button debounce, short/long press events
  led.[ch]       WS2812B status pulse
  main.c         app: modes; sand = sim task (core 0) + render task (core 1)
tools/
  sim_host.c     PC harness: repose angles, tilt/lean/flat behaviour, shake recovery
  sim_bench.c    PC benchmark: µs per step for settled / freefall / shake / flow
```

`sim`, `palette` and `render` only use the C standard library and `config.h`,
so they compile on a PC (`tools/sim_host.c`).

### Tasks / cores (sand mode)

| Task | Core | Rate | Work |
|---|---|---|---|
| `sim` | 0 | `SIM_HZ` (120 Hz), own clock | read IMU state, fixed sim steps, build snapshot |
| `imu` | 0 | `IMU_POLL_HZ` (200 Hz), prio above `sim` | I2C read, mapping, filtering, shake detection |
| `app` (render) | 1 | up to `TARGET_FPS` | button, render snapshot → frame buffer, DMA kick, LED, stats |
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
a grain leaving a cell within `WAKE_RADIUS` (2) of it, gravity moving by more
than `WAKE_GRAVITY_DELTA_G` (0.02 g, vector distance) or changing its snapped
direction / frozen state, a shake, or a respawn. A sleeper count lets moving
grains skip the neighbourhood scan entirely when nothing is asleep (the
worst case). Radius 1 / 24 steps left piles up to 4.5° steeper than without
sleep; 2 / 64 matches the no-sleep results within 0.8° on every host test.

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

- Black background. Palettes in `palette.c`, ~4 shades each:
  - **P1 green**: `#33FF33`, `#2BD92B`, `#5CFF5C`, `#22B822`
  - **P3 amber**: `#FFB000`, `#E69A00`, `#FFC233`, `#CC8800`
  - **orange** (gas-plasma): `#FF6A00`, `#E65A00`, `#FF8533`, `#C24E00`; LED `#FF2800`
- Each grain picks a random shade at spawn → textured piles.
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
- Default palette `PALETTE_DEFAULT` (green); BOOT long press cycles
  green → amber → orange.
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
| BOOT long press (≥ `BTN_LONG_MS`, fires while held) | cycle palette: green → amber → orange |

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
