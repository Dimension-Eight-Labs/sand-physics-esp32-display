# Phosphor Sand

Tilt-driven falling-sand demo for the **Waveshare ESP32-S3-LCD-1.47B**:
glowing monochrome grains in the style of an old phosphor terminal. Tilt to
pour, shake to splash. Design and hardware sources: [docs/SPEC.md](docs/SPEC.md).

## Using it

Plug in USB-C (or a battery) and the sand falls into place. Tilt the board to
pour, knock it to splash.

| BOOT button | Action |
|---|---|
| short press | respawn: the material drops in fresh from the top |
| long press (0.7 s) | next mode |

Six modes, in this order (the first is the one at power-up):

1. **green gravel**: coarse grains that bounce and pile at about 33°
2. **orange gravel**
3. **green sand**: open-hourglass sand: falls in a tidy stream that keeps its
   momentum and curves when you turn the board, piles at about 33°
4. **orange sand**
5. **green liquid**: a slightly viscous liquid that pours, sloshes once and
   settles level; a knock throws a small splash
6. **orange liquid**

Then back to green gravel. Switching modes keeps every grain where it is and
only changes colour and behaviour (a sand pile slumps flat as liquid; liquid
switched to sand piles up on the next tilt). The material name (GRAVEL / SAND
/ LIQUID) shows on screen for a moment after each switch and at power-up.

**Idle dimming.** Left alone for 3 minutes, the backlight fades down to a
glow and the LED dims, to save battery. Any real tilt, a knock or a button
press brings full brightness back at once; holding the board in your hand
(tremor, a few degrees of sway) does not keep it awake. Timeout, dim level
and an on/off switch are `IDLE_DIM_*` / `IDLE_WAKE_*` in `src/config.h`.

## Build, flash, monitor

PlatformIO environments (`sand` is the default for `pio run`):

```sh
pio run -e release     -t upload    # production build: warnings-only log, no stats line
pio run -e sand        -t upload    # development build of the same firmware (stats line every second)
pio run -e testpattern -t upload    # colour bars / offset / orientation check
pio run -e tiltdot     -t upload    # rolling dot + raw accelerometer log
pio device monitor                  # logs over native USB (USB-Serial-JTAG)
```

Build and upload, then monitor, in one go: `pio run -e tiltdot -t upload -t monitor`.

The firmware version lives in **`version.txt`** (now `1.0.0`); ESP-IDF puts
it in the app descriptor and the boot banner shows it. It does not depend on
git, so a clean checkout or a source archive builds the same version.

`release` is the demo built with `-DRELEASE_BUILD=1`: `sdkconfig.defaults.release`
is applied on top of `sdkconfig.defaults` (console log level warning, also
for the bootloader) and the once-a-second statistics line is off. The boot
banner is still printed. To get the statistics back in a release build, add
`-DSTATS_LOG=1` to `[env:release] build_flags`.

Port, monitor speed and filters are set in `platformio.ini` (`/dev/ttyACM0`).
If the port doesn't show up, hold BOOT while plugging in the cable.

Serial notes for this board (native USB, no UART chip):

- Every boot prints a banner with the mode, firmware version and build type
  (release / development), ESP-IDF version, IMU address/WHO_AM_I and free
  heap. Boot output is usually over before the monitor attaches, so the
  banner is printed **again whenever a monitor connects**.
- Pressing RESET (or any reboot) drops the USB device for a moment; the
  monitor may need restarting after that.
- `testpattern` prints an `alive` line every second, `tiltdot` prints the
  accelerometer 10× per second, `sand` prints timing stats every second
  (`release` only warnings).

PlatformIO platform `espressif32@7.0.1` (ESP-IDF 6.0.1). Each environment
gets its own generated `sdkconfig.<env>` (`sdkconfig.release`,
`sdkconfig.sand`, `sdkconfig.tiltdot`, `sdkconfig.testpattern`) created from
`sdkconfig.defaults` (plus `sdkconfig.defaults.release` for `release`). After
changing a defaults file, delete the generated files so the defaults are
applied again.

## Flashing a new board

**One merged image (no PlatformIO needed on the flashing machine).** Build
it once:

```sh
scripts/make_release.sh
```

This builds `env:release` and writes `dist/sand-physics-<version>.bin`
(`dist/sand-physics-1.0.0.bin`): bootloader at 0x0, partition table at
0x8000 and app at 0x10000, merged into one file that is written at offset
0x0. The offsets and flash settings (DIO, 80 MHz, 16 MB) come from the
build's `flasher_args.json`. The script prints the file's size and SHA-256.

Flash a board over its USB-C port:

```sh
esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash 0x0 dist/sand-physics-1.0.0.bin
```

That is esptool 4.x (`pip install "esptool<5"`, or PlatformIO's copy:
`~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py`).
With esptool 5.x the same command is
`esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0x0 dist/sand-physics-1.0.0.bin`.
On Windows the port is `COMx`, on macOS `/dev/cu.usbmodem…`. If the board
doesn't connect (an unknown or crashing firmware on it), hold BOOT while
plugging in the cable, flash, then press RESET. Boards that ran something
else before can be wiped first with `esptool.py --chip esp32s3 --port
/dev/ttyACM0 erase_flash` (this firmware stores nothing in flash, so it is
optional).

**With PlatformIO.** From a checkout:

```sh
pio run -e release -t upload
```

builds and writes bootloader, partition table and app to their offsets in one
go (same result as the merged image). Add `--upload-port <port>` if the board
is not on `/dev/ttyACM0`, and `-t erase` before `-t upload` to wipe it first.

## Configuration

Everything you are likely to change is in **`src/config.h`**. Pins and panel
settings are in `src/board.h`.

### Bring-up modes

Selected by environment (`-e testpattern | tiltdot | sand`), which sets
`APP_MODE` via `build_flags` in `platformio.ini`. Suggested order:

1. **`testpattern`** — bars top to bottom: white, yellow, cyan, green,
   magenta, red, blue, black (with a black-to-white grey ramp in the black
   bar); red square top-left, green top-right, blue bottom-left; a 1-px white
   border on all four edges; the RGB LED pulses red.
   - Edge missing / garbage strip → `BOARD_LCD_OFFSET_X/Y` in `board.h`.
   - Red and blue swapped → `BOARD_LCD_BGR`.
   - Colours rotated (red→blue, green→red, blue→green) and the grey ramp
     broken into coloured segments → `BOARD_LCD_SWAP_BYTES`.
   - LED not red → LED colour order in `src/led.c`.

   RGB LED in each mode: switched off as soon as the app starts (~0.3 s after
   reset, before the display is initialised), then pulsing red
   (`testpattern`), palette colour brightening as the dot rolls (`tiltdot`),
   palette colour following sand motion (`sand`).
   - Black shows white / negative image → `BOARD_LCD_INVERT`.
   - Mirrored → `BOARD_LCD_MIRROR_X/Y`.
   - Noise or tearing → lower `BOARD_LCD_SPI_HZ` to 40 MHz.
2. **`tiltdot`** — a dot rolls toward the lowest edge, using the same
   gravity model as the sand (in-plane pull plus friction against the glass).
   10× per second it prints raw chip axes, screen gravity, the in-plane angle
   (0° = toward the bottom edge, +90° right, −90° left, ±180° top), in-plane
   magnitude, glass friction and net pull. The mapping in `config.h`
   (confirmed: Y and Z; X sign flipped to −1 after the first test):
   ```c
   #define IMU_SCREEN_X_SRC  0     // 0 = chip X, 1 = chip Y, 2 = chip Z
   #define IMU_SCREEN_X_SIGN (-1)
   #define IMU_SCREEN_Y_SRC  1
   #define IMU_SCREEN_Y_SIGN (+1)
   #define IMU_SCREEN_Z_SRC  2     // screen normal; only |gz| is used (glass friction)
   #define IMU_SCREEN_Z_SIGN (+1)
   ```
   Held upright like a picture frame (USB at the bottom): angle ≈ 0°,
   magnitude ≈ 1 g, glass friction ≈ 0. Lowering the right edge: angle goes
   positive and the dot rolls right. Lying flat: magnitude ≈ 0, "held by
   friction".
3. **`sand`** — the demo.

`GRAIN_PX` and `PALETTE_DEFAULT` can likewise be overridden per environment
by adding e.g. `-DGRAIN_PX=1` to that environment's `build_flags`.

### Looks: palette + material

The long-press cycle is a table in `config.h`; reorder or extend it freely.
The first entry is the look at boot.

```c
#define SAND_MODE_TABLE {                                  \
    { "green gravel", PALETTE_GREEN, MATERIAL_CLASSIC },   \
    { "orange gravel", PALETTE_ORANGE, MATERIAL_CLASSIC }, \
    { "green sand", PALETTE_GREEN, MATERIAL_SAND },        \
    { "orange sand", PALETTE_ORANGE, MATERIAL_SAND },      \
    { "green liquid", PALETTE_GREEN, MATERIAL_LIQUID },    \
    { "orange liquid", PALETTE_ORANGE, MATERIAL_LIQUID },  \
}
```

Palettes (`PALETTE_GREEN`, `PALETTE_ORANGE`; 4 shades each plus the LED
colour) are in `src/palette.c`. `PALETTE_DEFAULT` only sets the tiltdot colour.

Materials are `material_t` structs in `config.h` (`MATERIAL_CLASSIC_PARAMS`
"gravel", `MATERIAL_SAND_PARAMS` "sand", `MATERIAL_LIQUID_PARAMS` "liquid"),
one engine with per-material parameters:

| Field | Effect |
|---|---|
| `rest_rule` | `MAT_REST_REPOSE` (gravel and sand pile at an angle) or `MAT_REST_SPREAD` (liquid levels) |
| `flow_model` | `MAT_FLOW_CLASSIC` (gravel: v1, grains bounce off the grain ahead) or `MAT_FLOW_COHERENT` (sand: a falling stream keeps together, updated along gravity, only the surface wakes on small tilts) |
| `repose_angle_deg`, `rule_prob` | steepest stable pile slope; how eagerly resting grains act |
| `gravity_scale`, `max_speed`, `damping` | how fast grains fall and flow |
| `elasticity`, `contact_friction` | bounce on impact, skid along the pile |
| `glass_friction` | friction against the glass × \|gz\|: slows grains when leaned back, freezes them near flat |
| `repose_dither` | randomness so pile angles aren't stuck on grid slopes |
| `spread_scan`, `spread_push` | liquid: how far a surface grain looks for a lower spot, how hard it is pushed there |
| `viscosity` | liquid: how strongly moving grains share momentum with their neighbours (0 = loose grains; 0.15 = slightly viscous, moves and falls as a body) |
| `wave_gain`, `wave_damping`, `wave_start` | liquid sloshing: how hard the surface is pulled back to level, how fast the waves die out (0.04 ≈ one gentle overshoot), how big a tilt starts them |
| `level_rate` | liquid: flow cap per column pair for the waves (grains/step) |
| `impact_absorb` | liquid landings: how strongly a landing grain takes on the velocity of the liquid around it (0.8: a pour joins the local flow, no bounce or splash) |
| `impact_to_flow` | liquid landings: extra sideways flow from the landing speed (0 = off; never faster than the landing) |
| `knock_splash`, `knock_fan_deg` | liquid knock: launch speed of thrown grains per unit of shake; spread of their launch directions (± degrees around up, leaning with the knock) |
| `knock_depth`, `knock_max`, `knock_interior` | liquid knock: how many grains from the top of each column can be thrown (1–2); most grains thrown per knock; sideways push on the rest of the body |
| `shade_spread` | shade variation: 1 = full palette texture (gravel), lower = smoother (sand 0.6, liquid 0.3) |

The knock is the liquid's only splash. Falling or poured liquid never
splashes: it is absorbed where it lands. Grains thrown by a knock are
*spray*: they land without any reaction, so a knock gives one splash that
falls back and settles.

**`MATERIAL_CLASSIC_PARAMS` ("gravel" on screen) is the sand confirmed on
hardware ("Classic physics v1" in `docs/SPEC.md`), verified bit for bit by
`tools/sim_golden.c`. Don't edit it; add a new material instead.**

### Grain size and amount

```c
#define GRAIN_PX      2      // pixels per grain: 2 → 86x160 grid, 1 → 172x320
#define FILL_FRACTION 0.33f  // fraction of cells holding sand
```

With `GRAIN_PX 1` the simulation memory (~420 KB) automatically goes to
PSRAM and the sim is ~4× more work per step; check the logged timings.

### Other physics and look settings

All in `config.h`, each commented:

| Parameter | Effect |
|---|---|
| `IMU_LPF_ALPHA` | gravity smoothing (0.15 at 200 Hz ≈ 30 ms): higher = snappier, lower = calmer |
| `WAKE_ANGLE_DEG`, `WAKE_PULL_DELTA_G` | how far gravity must move before sleeping grains are re-checked (2°, 0.06 g) |
| `SHAKE_THRESHOLD_G`, `SHAKE_STRENGTH` | when a shake counts, how hard it splashes |
| `GLOW_BASE`, `GLOW_SPEED_GAIN`, `HOT_WHITE_MIX` | settled vs. moving brightness |
| `PERSISTENCE_ENABLE`, `PERSISTENCE_KEEP` | phosphor afterglow on/off and length |
| `BACKLIGHT_PERCENT`, `LED_*` | backlight and RGB LED |

### Testing the simulation on a PC

`sim.c` and `material.c` have no ESP-IDF dependencies. Host tools:

Builds go to `build-host/` (git-ignored):

```sh
mkdir -p build-host
B="cc -std=c11 -O2 -Wall -Wextra -Isrc src/sim.c src/material.c"
$B tools/sim_golden.c   -lm -o build-host/sim_golden   && build-host/sim_golden   # CLASSIC == Classic physics v1, bit for bit
$B tools/sim_host.c     -lm -o build-host/sim_host     && build-host/sim_host     # sand: pile angles, lean, flat, tip, shake
$B tools/sim_liquid.c   -lm -o build-host/sim_liquid   && build-host/sim_liquid   # liquid levelling, material switching, tremor/wake
$B tools/sim_bench.c    -lm -o build-host/sim_bench    && build-host/sim_bench    # µs per step, both materials, 7 scenarios
$B tools/sim_hand.c     -lm -o build-host/sim_hand     && build-host/sim_hand     # held in hand: awake grains and cost over 20 s
$B tools/sim_liquidfx.c -lm -o build-host/sim_liquidfx && build-host/sim_liquidfx # liquid: drops, pours, sloshing, knock, flip
$B tools/sim_liquidstab.c -lm -o build-host/sim_liquidstab && build-host/sim_liquidstab # liquid always comes to rest: knock repro, 20 x 60 s random input
$B tools/sim_sandfx.c   -lm -o build-host/sim_sandfx   && build-host/sim_sandfx   # sand vs gravel: stream arc / width, heap angles, rest checks
$B src/idle.c tools/sim_idle.c -lm -o build-host/sim_idle && build-host/sim_idle  # idle dimming vs the hand-motion model
```

`sim_liquidfx`, `sim_liquidstab`, `sim_liquid`, `sim_bench` and `sim_hand` take liquid
(`sim_sandfx`: sand) parameter overrides as `name=value`, for any numeric `material_t` field
(`tools/overrides.h`), e.g. `build-host/sim_liquidfx wave_damping=0.02 viscosity=0.3`.
Each run prints the overrides it applied; an unknown name stops with an error
listing the valid ones. They only change the host run: to change the
firmware, edit `MATERIAL_LIQUID_PARAMS` in `config.h`.

## Serial output

In the `sand` environment (and `release` built with `-DSTATS_LOG=1`), once
per second (format; numbers illustrative):

```
fps 60.0 (60 new) | sim 120 steps/s, 820 us/step (max 3900), 4100 awake | snapshot 300 us | render 700 us | dma wait 0 us | motion 950
```

- `fps … (N new)`: frames drawn, and how many showed a new simulation state.
- `sim 120 steps/s`: the physics runs on core 0 on its own clock; 120 means
  real time. If a single step ever costs more than 1/120 s the line ends in
  `SIM BEHIND REAL TIME` and a warning says how much time was dropped.
- `us/step (max …)`, `awake`: step cost; resting grains fall asleep and cost
  almost nothing, so this is low when the sand is still.
- `snapshot`: time on the sim core to hand a frame to the renderer.
- `render`, `dma wait`: time on core 1 per frame.
- `| dimmed` at the end: idle dimming is active.

A `release` build prints only the boot banner and warnings, e.g. the
`simulation lost … ms of real time` warning if the sim ever falls behind.

The boot banner also shows where the simulation memory went; it should say
`sim … in internal RAM`.

## Performance notes

The ESP32-S3 FPU has no hardware divide or square root, so `sqrtf()`,
`floorf()`, `ceilf()` and `/` on floats are slow library calls. The
per-grain code avoids them entirely (see `docs/SPEC.md` §5, §10). To compare
step cost on a PC:

```sh
mkdir -p build-host && cc -std=c11 -O2 -Isrc src/sim.c src/material.c tools/sim_bench.c -lm -o build-host/sim_bench && build-host/sim_bench
```
