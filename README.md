# Phosphor Sand

Tilt-driven falling-sand demo for the **Waveshare ESP32-S3-LCD-1.47B**:
glowing monochrome grains in the style of an old phosphor terminal. Tilt to
pour, shake to splash. Design and hardware sources: [docs/SPEC.md](docs/SPEC.md).

## Build, flash, monitor

Each bring-up mode is its own PlatformIO environment (`sand` is the default):

```sh
pio run -e testpattern -t upload    # colour bars / offset / orientation check
pio run -e tiltdot     -t upload    # rolling dot + raw accelerometer log
pio run -e sand        -t upload    # the demo (same as plain `pio run -t upload`)
pio device monitor                  # logs over native USB (USB-Serial-JTAG)
```

Build and upload, then monitor, in one go: `pio run -e tiltdot -t upload -t monitor`.

Port, monitor speed and filters are set in `platformio.ini` (`/dev/ttyACM0`).
If the port doesn't show up, hold BOOT while plugging in the cable.

Serial notes for this board (native USB, no UART chip):

- Every boot prints a banner with the mode, firmware and ESP-IDF version,
  IMU address/WHO_AM_I and free heap. Boot output is usually over before the
  monitor attaches, so the banner is printed **again whenever a monitor
  connects**.
- Pressing RESET (or any reboot) drops the USB device for a moment; the
  monitor may need restarting after that.
- `testpattern` prints an `alive` line every second, `tiltdot` prints the
  accelerometer 10× per second, `sand` prints timing stats every second.

PlatformIO platform `espressif32@7.0.1` (ESP-IDF 6.0.1). Each environment
gets its own generated `sdkconfig.<env>` (`sdkconfig.sand`,
`sdkconfig.tiltdot`, `sdkconfig.testpattern`) created from
`sdkconfig.defaults`. After changing `sdkconfig.defaults`, delete those
generated files so the defaults are applied again.

## Controls

| BOOT button | Action |
|---|---|
| short press | respawn the sand at the top |
| long press (0.7 s) | next palette: P1 green → P3 amber → orange |

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

### Palette

```c
#define PALETTE_DEFAULT PALETTE_P1_GREEN   // or PALETTE_P3_AMBER, PALETTE_ORANGE
```

Colours (4 shades each, plus the LED colour) are in `src/palette.c`.

### Grain size and amount

```c
#define GRAIN_PX      2      // pixels per grain: 2 → 86x160 grid, 1 → 172x320
#define FILL_FRACTION 0.33f  // fraction of cells holding sand
```

With `GRAIN_PX 1` the simulation memory (~420 KB) automatically goes to
PSRAM and the sim is ~4× more work per step; check the logged timings.

### Physics and look

All in `config.h`, each commented. The ones that matter most:

| Parameter | Effect |
|---|---|
| `REPOSE_ANGLE_DEG` | steepest stable pile slope (default 33°) |
| `REPOSE_PROB` | how eagerly surface grains avalanche (speed, not final angle) |
| `GRAVITY_SCALE`, `MAX_SPEED`, `DAMPING` | how fast sand falls and flows |
| `ELASTICITY`, `CONTACT_FRICTION` | bounce on impact, skid along the pile |
| `BACK_FRICTION` | friction against the glass, × \|gz\|: slows sand when leaned back, freezes it near flat, steepens leaned-back piles |
| `REPOSE_DITHER` | randomness on the slide threshold so pile angles aren't stuck on grid slopes |
| `IMU_LPF_ALPHA` | gravity smoothing (0.15 at 200 Hz ≈ 30 ms): higher = snappier, lower = calmer |
| `SHAKE_THRESHOLD_G`, `SHAKE_STRENGTH` | when a shake counts, how hard it splashes |
| `GLOW_BASE`, `GLOW_SPEED_GAIN`, `HOT_WHITE_MIX` | settled vs. moving brightness |
| `PERSISTENCE_ENABLE`, `PERSISTENCE_KEEP` | phosphor afterglow on/off and length |
| `BACKLIGHT_PERCENT`, `LED_*` | backlight and RGB LED |

### Testing the simulation on a PC

`sim.c` has no ESP-IDF dependencies. The harness measures the pile angle,
stillness when flat, avalanche behaviour under tilt, and step time:

```sh
cc -std=c11 -O2 -Wall -Wextra -Isrc src/sim.c tools/sim_host.c -lm -o sim_host
./sim_host          # add "ascii" to print the heap
```

## Serial output

In `sand` mode, once per second (format; numbers illustrative):

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

The boot banner also shows where the simulation memory went; it should say
`sim … in internal RAM`.

## Performance notes

The ESP32-S3 FPU has no hardware divide or square root, so `sqrtf()`,
`floorf()`, `ceilf()` and `/` on floats are slow library calls. The
per-grain code avoids them entirely (see `docs/SPEC.md` §5, §10). To compare
step cost on a PC:

```sh
cc -std=c11 -O2 -Isrc src/sim.c tools/sim_bench.c -lm -o sim_bench && ./sim_bench
```
