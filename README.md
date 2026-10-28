# Phosphor Sand

Tilt-driven falling-sand demo for the **Waveshare ESP32-S3-LCD-1.47B**:
glowing monochrome grains in the style of an old phosphor terminal. Tilt to
pour, shake to splash. Design and hardware sources: [docs/SPEC.md](docs/SPEC.md).

## Using it

Plug in USB-C (or a battery) and the screen warms up like an old CRT: a dot,
a line, then the picture opens with the sand falling into place. Tilt the
board to pour, knock it to splash.

| BOOT button | Action |
|---|---|
| click (short press) | respawn: the material drops in fresh from the top. Happens 0.3 s after you let go, once it is clear no second click follows |
| double click | battery display: a battery with 5 bars (20 % each) for 3 s, over the running sand; on USB or charging the bars climb instead |
| long press (about 1 s, let go before 1.5 s) | next mode, when you let go |
| hold (3 s) | power off. From 1.5 s an `OFF` bar fills; let go while it shows to cancel (nothing happens), keep holding until it is full to power off |

The gestures cannot be mixed up: a click waits out the double-click window
before it counts, a long press only counts when you let go before the `OFF`
bar appears, and a click followed straight away by a long press or a hold
counts only as the long press or hold. Timings: `BTN_*` in `src/config.h`.

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

**Battery display.** Double-click BOOT to see the charge: a battery icon in
the current colour with 5 bars, each 20 % (1 bar from 10 %, 5 from 90 %),
over the running sand, fading out after 3 s. The level comes from the cell
voltage through a LiPo discharge curve, averaged and smoothed so it does
not flicker; it follows the battery down but only goes up again after a
clear rise, so the screen dimming (which lets the cell voltage recover a
little) does not make it jump. When the voltage is above a full cell's
(4.19 V) the board assumes it is on USB and the bars climb instead of
showing a level. The banner and the serial log (also on a double click)
show the voltage and percentage.

**Charging.** The firmware cannot tell reliably whether the battery is
charging: the charger's status pin only drives its own LED, and USB power
does not reach any GPIO. A part-charged cell on USB reads like a cell
running the board, and shows a level. **The onboard charge LED is the
charging indicator**: lit while the charger is charging, off when the cell
is full or there is no USB.

**Low battery.** Below 15 % (on battery) the RGB LED blinks red instead of
pulsing in the palette colour.

**Low-voltage cutoff.** When the cell falls below 3.30 V (measured while
running, smoothed over a minute) the empty battery icon blinks for 2 s with
the LED blinking red, the picture collapses, and the board sleeps, with
motion wake **off** so it cannot wake itself and drain the cell further.
BOOT or RESET still wake it; every boot first takes a settled voltage
reading with the backlight off, and below the cutoff it shows the blinking
empty battery and goes back to sleep without starting. After a cutoff the
cell must read at least 3.40 V to run again (a rested cell reads higher
than one under load, so this keeps it from switching on and straight off
again). Charge it over USB: the cutoff is not applied while the board
looks like it is on USB (a reading above 4.19 V, or a computer connected to
the USB serial port). On a plain USB charger with a deeply discharged cell
the board may keep refusing to start until the cell has charged above
3.40 V; the onboard charge LED shows that it is charging. Settings:
`BAT_CUTOFF_*` in `src/config.h`.

**Power off and on.** Hold BOOT for 3 s: the picture collapses like a CRT
switching off (it squeezes into a bright horizontal line, the line shrinks
to a dot, the dot fades) and the board goes into deep sleep: backlight off,
display controller asleep, LED dark, the motion sensor in its low-power
wake-on-motion mode. Let go of BOOT after the dot has gone; the board
sleeps once the button is up. It wakes, with the warm-up animation, on:

- a **BOOT press**;
- **RESET**;
- **motion**: picking the board up or tilting it, from 10 s after power-off
  (so setting it down right after a hold-to-off does not wake it again).

**Auto-off.** Left alone for 15 minutes it powers off by itself, with the
same animation. "Alone" means what it means for dimming: no knock, no real
tilt, no sand still moving, no button press; hand tremor and desk vibration
do not count, so a board held still in the hand also powers off. Dimming at
3 minutes stays as it is. `AUTO_OFF_ENABLE`, `AUTO_OFF_S` in `src/config.h`.

**Mode memory.** Waking from power-off (button, motion) returns to the mode
you powered off in, with fresh sand. A cold power-up (battery or USB
connected) or RESET starts in the first mode (green gravel); the saved mode
lives in RTC memory, which only survives deep sleep.

**Wake on motion.** The QMI8658 watches for motion on its own while the
board sleeps and wakes it through its INT2 line (GPIO12). The threshold,
`WOM_THRESHOLD_MG` (150 mg at 11 Hz to start with), is meant to ignore light
bumps on the desk; raise it if the board wakes from bumps, lower it if a
gentle pick-up does not wake it. `WOM_ENABLE` turns motion wake off.

## Build, flash, monitor

PlatformIO environments (`sand` is the default for `pio run`):

```sh
pio run -e release     -t upload    # production build: warnings-only log, no stats line
pio run -e sand        -t upload    # development build of the same firmware (stats line every second)
pio run -e testpattern -t upload    # colour bars / offset / orientation check
pio run -e tiltdot     -t upload    # rolling dot + raw accelerometer log
pio run -e ledcal      -t upload    # palette swatch + candidate LED colours (LED colour matching)
pio device monitor                  # logs over native USB (USB-Serial-JTAG)
```

Build and upload, then monitor, in one go: `pio run -e tiltdot -t upload -t monitor`.

The firmware version lives in **`version.txt`** (now `1.2.0`); ESP-IDF puts
it in the app descriptor and the boot banner shows it. It does not depend on
git, so a clean checkout or a source archive builds the same version.
Changing `version.txt` re-runs the CMake configure step on the next build
(`CMakeLists.txt`), so incremental builds pick up a new version too.

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
`sdkconfig.sand`, `sdkconfig.tiltdot`, `sdkconfig.testpattern`,
`sdkconfig.ledcal`) created from
`sdkconfig.defaults` (plus `sdkconfig.defaults.release` for `release`). After
changing a defaults file, delete the generated files so the defaults are
applied again.

## Flashing a new board

The release image is one merged file, `dist/sand-physics-1.2.0.bin`
(bootloader at 0x0, partition table at 0x8000, app at 0x10000), written at
offset 0x0. To build it from a checkout:

```sh
scripts/make_release.sh
```

This builds `env:release` and writes `dist/sand-physics-<version>.bin`. The
offsets and flash settings (DIO, 80 MHz, 16 MB) come from the build's
`flasher_args.json`. The script prints the file's size and SHA-256.

**Flash with PlatformIO's bundled esptool** (this method was verified on
hardware with the 1.0.0 release image; no separate install needed). Connect the board's USB-C
port and run:

```sh
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash 0x0 dist/sand-physics-1.2.0.bin
```

Other ways to run the same flash, in order of preference:

1. **If those paths don't exist** (PlatformIO installed elsewhere), let
   PlatformIO find its esptool:

   ```sh
   pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash 0x0 dist/sand-physics-1.2.0.bin
   ```

2. **A standalone esptool.** On Debian, `sudo apt install esptool`. The
   command is then `esptool` without `.py`:

   ```sh
   esptool --chip esp32s3 --port /dev/ttyACM0 write_flash 0x0 dist/sand-physics-1.2.0.bin
   ```

   esptool 5 spells the subcommand `write-flash` (with a hyphen): `esptool
   --chip esp32s3 --port /dev/ttyACM0 write-flash 0x0 dist/sand-physics-1.2.0.bin`.
   Check yours with `esptool version`.

3. **Build and upload with PlatformIO** from a checkout, without the merged
   image:

   ```sh
   pio run -e release -t upload
   ```

   This builds and writes the bootloader, partition table and app to their
   offsets in one go, the same result as the merged image. Add
   `--upload-port <port>` if the board is not on `/dev/ttyACM0`, and
   `-t erase` before `-t upload` to wipe it first.

On Windows the port is `COMx`, on macOS `/dev/cu.usbmodem…`. If the board
doesn't connect (an unknown or crashing firmware on it), hold BOOT while
plugging in the cable, flash, then press RESET. A board that ran something
else before can be wiped first with the same tool and `erase_flash` (esptool
5: `erase-flash`). This is optional, because this firmware stores nothing in
flash.

### Verifying a new board

After flashing, open a serial monitor (`pio device monitor`, or any terminal
on `/dev/ttyACM0`) and check:

- [ ] The boot banner shows `firmware  : 1.2.0` and
      `build     : release (log level warning, stats log off)`. The banner is
      printed again whenever a monitor connects.
- [ ] No once-a-second stats line (`fps … | sim … steps/s …`) appears after
      the banner.
- [ ] The sand falls, follows tilts, and a long press on BOOT cycles the six
      modes. If the display looks wrong (colours, offset, orientation),
      flash the test pattern with `pio run -e testpattern -t upload` and
      compare it with the expected bars and corner markers; flash the
      release image again afterwards.
- [ ] Left alone for 3 minutes, the screen dims and the LED goes down; a
      knock brings full brightness back at once.
- [ ] Holding BOOT shows the `OFF` bar from 1.5 s; at 3 s the picture
      collapses to a dot and the board sleeps (the console prints
      `power off: …`). A BOOT press wakes it with the warm-up animation, in
      the same mode (`started by: BOOT button`, `restored after power-off`
      in the banner).
- [ ] Powered off and left still for more than 10 s, picking the board up
      wakes it (`started by: motion`).

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
   - LED not red → LED byte order, `BOARD_RGB_ORDER_RGB` in `src/board.h`
     (the `ledcal` environment's start-up check shows all three channels).

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

### Power

All in `config.h`, "Button (BOOT)" and "Power":

| Parameter | Effect |
|---|---|
| `BTN_DOUBLE_MS`, `BTN_LONG_MS`, `BTN_HOLD_SHOW_MS`, `BTN_POWER_OFF_MS` | double-click window (300 ms), long press (1 s), when the `OFF` bar appears (1.5 s), power off (3 s) |
| `AUTO_OFF_ENABLE`, `AUTO_OFF_S` | auto-off on/off, after how long without activity (15 min) |
| `WOM_ENABLE`, `WOM_THRESHOLD_MG`, `WOM_ODR` | wake on motion on/off, its threshold (mg, 1..255) and the sensor's low-power rate |
| `WOM_ARM_DELAY_S` | motion wake starts this long after power-off (10 s) |
| `SLEEP_LED_DATA_LOW` | LED data line level in sleep (see below) |
| `CRT_OFF_*_MS`, `CRT_ON_*_MS` | the phases of the two animations (0.7 s off, 0.65 s on) |

Hardware facts these rely on, checked against the Waveshare schematic
(`ESP32-S3-LCD-1.47B_schematic_diagram.pdf`) and noted in `src/board.h`:
QMI8658 INT1 → GPIO13, INT2 → GPIO12 (both RTC-capable); BOOT on GPIO0 with
a 10 kΩ pull-up; RESET on CHIP_PU. The Waveshare demos don't use the IMU
interrupts.

Waking on a BOOT press is safe although GPIO0 is a strapping pin: the
ESP32-S3 latches its strapping pins only at a chip or system reset
(power-up, RESET, brown-out, RTC watchdog), and a deep-sleep wake is a core
reset (ESP32-S3 datasheet, "Strapping Pins"; Technical Reference Manual,
"Reset and Clock", table 7-1), so BOOT held during a wake cannot start
download mode. Holding BOOT while pressing RESET or plugging in still enters
download mode, as intended.

The LED's data line has a 10 kΩ pull-up to 3V3 on the board (R21). Holding
it low in sleep would draw 330 µA the whole time, so by default it is held
high, which costs nothing and starts no frame (a WS2812 latches only at the
end of a low reset pulse). `SLEEP_LED_DATA_LOW 1` holds it low instead.

### Battery

All in `config.h`, "Battery":

| Parameter | Effect |
|---|---|
| `BAT_ADC_SCALE` | divider correction (1.0072, Waveshare's demo value); compare the banner voltage with a meter on the cell and adjust |
| `BAT_FILTER_S`, `BAT_ADC_SAMPLES`, `BAT_SAMPLE_MS` | smoothing (60 s), conversions averaged per reading (16), reading interval (1 s) |
| `BAT_HYST_PERCENT`, `BAT_RISE_PERCENT` | how far the reading must fall (2 %) or rise (10 %) before the shown level follows |
| `BAT_CHARGING_MV` | above this (4190 mV) the board assumes USB / charging |
| `BAT_OSD_MS`, `BAT_OSD_FADE_MS`, `BAT_OSD_CHARGE_STEP_MS` | battery display time (3 s), fade (0.4 s), climbing-bar step (0.3 s) |
| `BAT_LOW_ENABLE`, `BAT_LOW_PERCENT`, `BAT_LOW_BLINK_*`, `BAT_LOW_COLOR` | low-battery LED blink: on/off, threshold (15 %), timing, colour |

| `BAT_CUTOFF_ENABLE`, `BAT_CUTOFF_MV`, `BAT_CUTOFF_RESUME_MV` | low-voltage cutoff on/off, threshold (3300 mV), extra margin to run again after a cutoff (100 mV) |
| `BAT_CUTOFF_WARN_MS`, `BAT_CUTOFF_BLINK_MS` | how long the empty battery blinks (2 s), blink period |

**Calibrating `BAT_ADC_SCALE` against a meter.** The ADC (even calibrated)
and the 1 % divider resistors can be off by a percent or two, about
40–80 mV at the cell.

1. Run the board on battery (USB unplugged, or the reading is the charger's).
2. Measure the cell with a multimeter at the battery connector, and read
   the board's value at the same time: double-click BOOT (`battery: 3.95 V,
   …` in the log) or watch `bat … mV` in the stats line of the `sand`
   environment. The firmware value is smoothed over a minute, so leave the
   board alone (no mode changes, same brightness) for a minute first.
3. New `BAT_ADC_SCALE` = old value × meter / firmware. For example 1.0072 ×
   3.912 / 3.950 = 0.9975. Rebuild; repeat once at a different charge level
   if you want to check it.

**Reading the drain log.** At every power-off (BOOT hold, auto-off or
cutoff) the board stores the battery voltage and the time in RTC memory. On
the next wake from that sleep the boot banner shows them against a fresh
reading:

```
 last off  : BOOT held, slept 26 h 40 min, 3.952 V -> 3.931 V, drain 18.9 mV/day
```

- Both readings are settled, unsmoothed averages (64 conversions after the
  ADC has settled), taken with the backlight off: at power-off after the
  screen is dark, at wake before the backlight comes on.
- `drain … mV/day` is the average fall per day while asleep. Sleeps under
  an hour print `too short to estimate a drain`: two readings agree only to
  a few mV.
- To turn it into a current: on the flat middle of the LiPo curve (about
  3.75–3.95 V) 1 % of capacity is roughly 3–5 mV, so a 500 mAh cell losing
  20 mV/day loses about 4–7 % (20–35 mAh) per day, or roughly 1 mA. Near
  full or near empty the curve is steeper and the same mV means less charge.
- A cell that has just been under load recovers for a while after the
  screen goes dark, so a short sleep after heavy use can even read a small
  negative drain. Sleeps of a day or more give the most useful numbers.
- The sleep time comes from the chip's RTC, which runs on its internal RC
  oscillator (this board has no 32 kHz crystal): expect it to be off by a
  few percent.
- RESET or a power loss clears the record (`last off : none`).

The banner is printed again whenever a serial monitor connects, so you can
wake the board on battery and plug in USB afterwards to read it; the
numbers were taken at the wake and do not change with the charging that
follows.

Hardware, checked against the schematic: BAT → R1 200 kΩ / R4 100 kΩ (1 %)
→ BAT_ADC on GPIO1 (ADC1 channel 0), with C8 100 nF; the divider is on the
cell side of the battery switch, so it reads the cell also on USB. The
ETA6098 charger's STAT pin only drives the charge LED, and VBUS reaches no
GPIO. The ADC runs at 6 dB attenuation (up to about 1.75 V; the divider
gives at most 1.4 V) with ESP-IDF's curve-fitting calibration. Readings are
taken by a low-priority task on core 1 (the simulation runs on core 0),
about 1 ms of work once a second.

### Matching the LED colour

The RGB LED has its own colour per palette, `LED_COLOR_GREEN` and
`LED_COLOR_ORANGE` in `config.h`, separate from the screen colours: the
WS2812's green is bluer and much brighter than the panel's, so copying the
screen colour does not look the same. The values are gamma-encoded like a
screen colour, plain `0xRRGGBB`; each channel is linearised once with
`LED_GAMMA` (2.2), after any adjustment, and brightness (pulse, dimming,
power animations) then scales all three linear channels together, so it
does not shift the hue (`src/ledcolor.c`, checked by `tools/sim_ledcolor.c`). The shipped values
(`0x2DFF00`, `0xFF9C00`) were matched by eye on hardware with `ledcal`.

To find better ones:

1. `pio run -e ledcal -t upload -t monitor`.
2. **Channel check** first: the screen and the LED show pure red, then
   pure green, then pure blue, one second each, with the name on screen and
   in the log (`ledcal: channel check RED …`). The LED must match the screen
   each time. If red and green come out swapped, flip `BOARD_RGB_ORDER_RGB`
   in `src/board.h` (the LED's byte order on the data line; the only place
   channel order is handled) and rebuild.
3. Then calibration starts: the screen fills with the palette's main colour
   (the palette name shows for a moment); the LED shows a candidate colour,
   at the brightness of moving sand.
4. **Click** for the next candidate, **double click** for the previous one.
   Candidates step the hue in 3° steps from −15° to +15° around the
   configured colour, then repeat with the saturation lowered by 0.1, down
   to −0.4 (55 candidates, the first one shown is the configured colour).
   **Long press** switches palette (green, orange) and starts again at the
   configured colour.
5. Every change prints a line like
   `ledcal green: candidate 7/55, hue +3 deg, saturation 1.00 -> #define LED_COLOR_GREEN 0x3AFF00 …`.
   Copy the `#define` of the best match into `src/config.h`, rebuild the
   release image.

Look at the LED and the screen side by side, from a normal viewing
distance; the step sizes and range are `LEDCAL_*` in `config.h`.

### Sleep current

Expected deep-sleep current from the battery, from datasheets and the
schematic (typical values; nothing here has been measured on this board):

| Part | Sleep current | Source |
|---|---|---|
| WS2812B LED (powered from 3V3, no switch; dark) | 0.4–1 mA | no datasheet figure; published bench measurements of WS2812B parts |
| ME6217C33 LDO quiescent (enable tied to VSYS, always on) | 100 µA (130 max) | ME6217 datasheet, ISS1 |
| QMI8658 wake-on-motion, accelerometer only, 11 Hz low-power | 35 µA | QMI8658C datasheet rev 0.6, table 14 (at 1.8 V; the board runs it at 3.3 V) |
| Battery divider R1 200K + R4 100K to BAT_ADC | 12–14 µA | schematic, 3.7–4.2 V / 300 kΩ |
| ST7789 display controller in sleep-in, backlight FET off | ~10 µA | typical ST7789 sleep-in figure (not from this panel's datasheet) |
| ESP32-S3 deep sleep, RTC memory on | 7–8 µA | ESP32-S3 datasheet, table 4-8 |
| ETA6098 charger, no USB | 0–5 µA | ETA6098 datasheet not found; similar ETA chargers specify ≤ 5 µA battery leakage |
| MBR230LSF diode reverse leakage (VSYS → VBUS → 101 kΩ) | a few µA | not checked |
| Flash, PSRAM (on VDD_SPI, switched off in deep sleep) | 0 | ESP32-S3 datasheet |
| LED data line held high against its pull-up, other held pins | 0 | |
| **Total** | **about 0.6–1.2 mA**, of which about 0.17 mA without the LED | |

The LED alone draws most of it, and it has no power switch on this board.
At about 1 mA a 500 mAh cell lasts roughly three weeks powered off. An SD
card left in the slot adds its own standby current (typically 0.1 mA or
more).

**Measuring it.** Put a multimeter in series with the battery and run on
the battery alone:

1. Unplug USB (with USB connected, VBUS powers the board and the battery
   charges).
2. Unplug the battery lead and connect the meter between the battery's
   positive wire and the board's battery positive pin (black lead to the
   board side). Start on the **mA** range: booting draws 100 mA or more,
   and the µA range's internal resistance can drop the voltage enough to
   brown out the board. A short wire across the meter's terminals while
   the board starts, removed once it runs, does the same job.
3. Power the board off with a 3 s hold on BOOT, keep it still, and wait
   15 s (the board wakes briefly 10 s after power-off to arm motion wake).
4. Switch to the µA range (or the 2 mA / 4 mA range) and read. Expect
   roughly 0.6–1.2 mA. Don't touch or move the board while switching ranges,
   or the motion wake may start it; check the screen stays dark.

Compare with the reading while running (backlight on, about 100 mA or more)
and while dimmed.

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
$B src/ledcolor.c src/palette.c tools/sim_ledcolor.c -lm -o build-host/sim_ledcolor && build-host/sim_ledcolor # LED colour: channel order, gamma once, dimming keeps hue
$B src/batt.c tools/sim_battery.c -lm -o build-host/sim_battery && build-host/sim_battery # battery: curve, no jitter under noise, discharge, charging hint
$B -Itools src/idle.c src/gesture.c tools/sim_power.c -lm -o build-host/sim_power && build-host/sim_power # BOOT gestures (none confusable), auto-off
mkdir -p build-host/crt && $B src/palette.c src/render.c tools/crt_preview.c -lm -o build-host/crt_preview && build-host/crt_preview # CRT animations, OFF bar, battery display as PPM frames in build-host/crt/
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
fps 60.0 (60 new) | sim 120 steps/s, 820 us/step (max 3900), 4100 awake | snapshot 300 us | render 700 us | dma wait 0 us | motion 950 | bat 3950 mV 70 %
```

- `fps … (N new)`: frames drawn, and how many showed a new simulation state.
- `sim 120 steps/s`: the physics runs on core 0 on its own clock; 120 means
  real time. If a single step ever costs more than 1/120 s the line ends in
  `SIM BEHIND REAL TIME` and a warning says how much time was dropped.
- `us/step (max …)`, `awake`: step cost; resting grains fall asleep and cost
  almost nothing, so this is low when the sand is still.
- `snapshot`: time on the sim core to hand a frame to the renderer.
- `render`, `dma wait`: time on core 1 per frame.
- `bat … mV … %`: filtered cell voltage and shown level; `(USB)` when it
  reads as on USB / charging, `LOW` below the low-battery threshold.
- `| dimmed` at the end: idle dimming is active.

A `release` build prints only the boot banner and warnings, e.g. the
`simulation lost … ms of real time` warning if the sim ever falls behind.
Every build also prints `battery: 3.95 V, 70 %, 4 of 5 bars` on a double click and
`power off: wakes on BOOT or RESET, and on motion after 10 s` just
before sleeping. The banner's `started by:` line says why the board started
(`power-on / RESET`, `BOOT button`, `motion`, …).

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
