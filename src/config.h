// All user-tunable parameters for the demo, in one place.
//
// This header is included by the hardware-free modules (sim, material,
// palette, render), so it must only contain macros and type definitions.
#pragma once

#include "board.h"

// Hot per-grain / per-pixel functions run from IRAM on the ESP32-S3 instead of
// through the flash cache that both cores share. No effect on a PC build.
#if defined(ESP_PLATFORM)
#define HOT_FN __attribute__((section(".iram1.hot")))
#else
#define HOT_FN
#endif

// ---- Application mode -------------------------------------------------------
#define MODE_TEST_PATTERN 0   // colour bars + border: check offset/orientation/colour
#define MODE_TILT_DOT     1   // one rolling dot + raw accel log: check IMU mapping
#define MODE_SAND         2   // the full demo
#define MODE_LED_CAL      3   // palette swatch + candidate LED colours: match the RGB LED to the screen

#ifndef APP_MODE
#define APP_MODE MODE_SAND
#endif

// ---- Screen / grid ------------------------------------------------------------
#define SCREEN_W BOARD_LCD_WIDTH
#define SCREEN_H BOARD_LCD_HEIGHT

// Size of one grain in pixels (1 = one grain per pixel). 172x320 divided by
// GRAIN_PX gives the grid; any leftover pixels on the right/bottom stay black.
#ifndef GRAIN_PX
#define GRAIN_PX 2
#endif
#define GRID_W (SCREEN_W / GRAIN_PX)
#define GRID_H (SCREEN_H / GRAIN_PX)

// Fraction of grid cells filled with sand.
#define FILL_FRACTION 0.33f
// On respawn, grains are scattered randomly over the top part of the screen
// covering this fraction of the height (must be > FILL_FRACTION).
#define SPAWN_REGION_FRACTION 0.55f

// ---- Timing -------------------------------------------------------------------
#define TARGET_FPS              60
#define SIM_HZ                  120    // fixed simulation rate, independent of fps
// The simulation runs on its own core and clock (MODE_SAND), so slow frames
// never slow the physics. Only if one step costs more than 1/SIM_HZ can it
// fall behind; then at most this much backlog is kept and the rest is dropped
// (and logged), rather than stepping in an ever-growing burst.
#define SIM_MAX_BACKLOG_MS      100

// ---- Materials (units: cells and simulation steps) ------------------------------
// All per-material physics lives in one struct. The simulation engine is the
// same for every material; the resting rule decides what a grain that sits on
// the pile does: REPOSE (sand: slide only if the slope is steeper than the
// angle of repose) or SPREAD (liquid: flow sideways toward any lower spot, so
// the surface levels perpendicular to gravity).
#define MAT_REST_REPOSE 0
#define MAT_REST_SPREAD 1

// How a material moves (MAT_REST_REPOSE materials; liquid always uses the
// coherent path).
#define MAT_FLOW_CLASSIC  0 // gravel: Classic physics v1, bounces off the grain ahead
#define MAT_FLOW_COHERENT 1 // sand: keeps pace with the grain ahead, updated along gravity

typedef struct {
    const char *name;
    // Motion
    float gravity_scale;    // cells/step^2 per g of in-plane gravity
    float damping;          // fraction of velocity lost per step while moving (drag)
    float elasticity;       // velocity kept (and reversed) on a blocked axis
    float max_speed;        // cells/step; moves above 1 cell/step are sub-stepped (<= 8)
    float jitter;           // +/- cells/step random kick for grains faster than rest_speed
    float contact_friction; // tangential velocity lost per step when moving on the pile
    float rest_speed;       // supported grains slower than this rest (> gravity_scale)
    float glass_friction;   // Coulomb friction against the glass per g of |g_z|
    float rule_token;       // velocity (x rest_speed) given after a resting-rule move
    // Resting rule
    int rest_rule;          // MAT_REST_REPOSE or MAT_REST_SPREAD
    float rule_prob;        // chance per step (x min(1, sqrt(net pull))) to apply it
    // MAT_REST_REPOSE
    float repose_angle_deg; // steepest stable slope upright
    int repose_scan;        // cells sideways/down the slope is measured over (1..6)
    float repose_diag_correction_deg; // threshold reduction in diagonal gravity frames
    float repose_dither;    // +/- fraction of random variation on the threshold
    // MAT_REST_SPREAD
    int spread_scan;        // cells sideways a grain looks for somewhere lower
    float spread_push;      // cells/step sideways velocity given toward it
    int spread_step;        // cells it may jump toward it in one step (flow rate)
    int level_rate;         // levelling moves per step (global) / per column pair (waves); 0 = off
    float wave_gain;        // >0: waves instead of global levelling; flow gained per cell of height difference
    float wave_damping;     // fraction of the column-to-column flow lost per step
    int wave_window;        // columns averaged on each side for the wave drive (>= width: whole tank)
    float wave_start;       // waves start only above this imbalance (cells); smaller: quiet levelling
    int level_global_rate;  // with waves: background global levelling moves per step
    int level_range;        // how many lines across gravity such a move may span
    float level_max_motion; // skip levelling while total motion is above this (splashing)
    // Sleep
    int wake_radius;        // cells around a vacated cell whose sleepers wake (0 = WAKE_RADIUS)
    // Motion model (MAT_FLOW_*; 0 = classic)
    int flow_model;
    // Liquid behaviour (MAT_REST_SPREAD only; 0 = off). See docs/SPEC.md §5b.
    float viscosity;        // per step, velocity pulled this far toward the liquid neighbours' mean
    float impact_to_flow;   // landing speed turned into sideways flow (< 1; never above the landing speed)
    float impact_absorb;    // on landing, velocity pulled this far toward the liquid neighbours' mean
    float knock_splash;     // launch speed of thrown surface grains, cells/step per unit of shake strength
    float knock_fan_deg;    // launch directions spread +/- this far around the splash direction
    int knock_depth;        // grains from the top of each column that can be thrown (1..2)
    int knock_max;          // most grains thrown by one knock
    float knock_interior;   // sideways knock push on the rest of the body, x knock_splash
    // Look
    float shade_spread;     // 1 = full palette shade variation, 0 = one shade
} material_t;

#define MATERIAL_CLASSIC 0
#define MATERIAL_LIQUID  1
#define MATERIAL_SAND    2
#define MATERIAL_COUNT   3

// CLASSIC = "Classic physics v1" (docs/SPEC.md), the sand confirmed on
// hardware, shown to the user as "gravel". tools/sim_golden.c checks it still
// matches bit for bit: do not change these values; make a new material instead.
#define MATERIAL_CLASSIC_PARAMS {                                                   \
    .name = "gravel",                                                               \
    .gravity_scale = 0.06f, .damping = 0.01f, .elasticity = 0.30f,                  \
    .max_speed = 2.0f, .jitter = 0.01f, .contact_friction = 0.30f,                  \
    .rest_speed = 0.15f, .glass_friction = 0.15f, .rule_token = 0.8f,               \
    .rest_rule = MAT_REST_REPOSE, .rule_prob = 0.35f,                               \
    .repose_angle_deg = 33.0f, .repose_scan = 4,                                    \
    .repose_diag_correction_deg = 4.0f, .repose_dither = 0.15f,                     \
    .spread_scan = 0, .spread_push = 0.0f, .spread_step = 0,                        \
    .level_rate = 0, .level_range = 0, .level_max_motion = 0.0f,                    \
    .wake_radius = 0,                                                               \
    .shade_spread = 1.0f,                                                           \
}

// LIQUID (called "water" in earlier builds): same engine, no angle of repose, slightly
// viscous. Low bounce and almost no friction on the pile; neighbours share
// momentum (viscosity), so the body moves and falls as a body. A landing grain
// is absorbed into the local flow (impact_absorb) instead of bouncing or
// splashing; the waves make the body slosh past level once and settle.
// Resting surface grains on solid ground flow toward lower spots up to
// spread_scan cells away. Only a knock splashes: a capped number of surface
// grains thrown in a fan; they are spray and are absorbed when they land.
// Less glass friction than sand, but enough to lie still when flat. Shades
// pulled together for a smoother body.
#define MATERIAL_LIQUID_PARAMS {                                                     \
    .name = "liquid",                                                                \
    .gravity_scale = 0.06f, .damping = 0.005f, .elasticity = 0.05f,                 \
    .max_speed = 2.0f, .jitter = 0.01f, .contact_friction = 0.02f,                  \
    .rest_speed = 0.15f, .glass_friction = 0.08f, .rule_token = 0.8f,               \
    .rest_rule = MAT_REST_SPREAD, .rule_prob = 1.0f,                                \
    .repose_angle_deg = 0.0f, .repose_scan = 0,                                     \
    .repose_diag_correction_deg = 0.0f, .repose_dither = 0.0f,                      \
    .spread_scan = 24, .spread_push = 0.4f, .spread_step = 0,                       \
    .level_rate = 3, .level_range = 86, .level_max_motion = 400.0f,                 \
    .wave_gain = 0.05f, .wave_damping = 0.04f, .wave_window = 1000,                 \
    .wave_start = 4.0f,                                                             \
    .level_global_rate = 8,                                                         \
    .wake_radius = 1,                                                               \
    .viscosity = 0.15f, .impact_to_flow = 0.0f, .impact_absorb = 0.8f,              \
    .knock_splash = 1.0f, .knock_fan_deg = 35.0f, .knock_depth = 1,                 \
    .knock_max = 40, .knock_interior = 0.1f,                                        \
    .shade_spread = 0.3f,                                                           \
}

// SAND: open-hourglass sand. Piles like gravel (angle of repose 33°, the same
// lean / glass friction behaviour), but moves on the coherent path liquid
// uses: a grain blocked by a grain moving the same way keeps pace with it
// instead of bouncing off, grains are updated farthest along gravity first,
// only grains grounded through a still chain to a wall rest, and small
// gravity changes wake only the exposed grains. So a falling stream stays
// together, and grains in flight keep their momentum: a higher speed cap
// (sub-stepped, never more than one cell per sub-step) and less drag mean a
// turn of the board curves a falling stream instead of snapping it. Less
// bounce than gravel; nothing adds energy on landing (no splash, no flow).
#define MATERIAL_SAND_PARAMS {                                                      \
    .name = "sand",                                                                 \
    .gravity_scale = 0.06f, .damping = 0.002f, .elasticity = 0.15f,                 \
    .max_speed = 4.0f, .jitter = 0.005f, .contact_friction = 0.30f,                 \
    .rest_speed = 0.15f, .glass_friction = 0.15f, .rule_token = 0.8f,               \
    .rest_rule = MAT_REST_REPOSE, .rule_prob = 0.35f,                               \
    .repose_angle_deg = 33.0f, .repose_scan = 4,                                    \
    .repose_diag_correction_deg = 4.0f, .repose_dither = 0.15f,                     \
    .wake_radius = 0,                                                               \
    .flow_model = MAT_FLOW_COHERENT,                                                \
    .shade_spread = 0.6f,                                                           \
}

// In-plane gravity below this (g, after glass friction) freezes the pile:
// it holds its shape when the board lies (nearly) flat.
#define GRAVITY_DEADZONE  0.03f

// ---- Performance: sleeping grains -----------------------------------------------
// A grain that hasn't moved for SLEEP_STEPS steps is skipped until something
// can change its situation: a grain leaving a cell within WAKE_RADIUS of it,
// a shake, a respawn, a material change, or gravity moving far enough from
// where it was when the grains last went to sleep: its in-plane direction
// turning by WAKE_ANGLE_DEG, its in-plane or normal magnitude changing by
// WAKE_PULL_DELTA_G, or the pile freezing/unfreezing. Small hand tremor stays
// inside these limits, so a held pile can sleep; a deliberate tilt crosses
// them at once. With 64 / 2 the host tests match running without sleep to
// within ~0.8°; radius 1 left piles up to 4.5° too steep.
#ifndef SLEEP_STEPS
#define SLEEP_STEPS          64
#endif
#ifndef WAKE_RADIUS
#define WAKE_RADIUS          2
#endif
#ifndef WAKE_ANGLE_DEG
#define WAKE_ANGLE_DEG       2.0f
#endif
// Liquid levelling is re-run only when grains moved or gravity turned by more
// than this since the last pass that found the surface level.
#define LEVEL_RECHECK_DEG    0.5f
#ifndef WAKE_PULL_DELTA_G
#define WAKE_PULL_DELTA_G    0.06f
#endif

// ---- Shake ----------------------------------------------------------------------
// | |a| - 1 g | above this (g) counts as a shake.
#define SHAKE_THRESHOLD_G 0.60f
// Impulse (cells/step) per g of shake excess.
#define SHAKE_STRENGTH    0.9f

// ---- IMU -----------------------------------------------------------------------
#define IMU_POLL_HZ   200
// One-pole low-pass on the gravity vector (each component, so no angle
// wraparound): y += alpha * (x - y) per sample. At 200 Hz, 0.15 is a ~30 ms
// time constant: steering feels immediate, sensor noise is smoothed out.
#define IMU_LPF_ALPHA 0.15f

// Axis mapping from QMI8658 axes to screen axes (screen: x right, y down,
// z out of the glass toward the viewer). SRC: 0 = chip X, 1 = chip Y,
// 2 = chip Z. SIGN: +1 or -1. Verified with the tiltdot environment: held
// upright like a picture frame (USB at the bottom) raw ay = -0.9 g, az = 0, and
// the dot falls to the bottom. Rotating in the screen plane, the dot moved
// away from the lowered edge with X sign +1, so X is now -1 (to re-check).
#define IMU_SCREEN_X_SRC  0
#define IMU_SCREEN_X_SIGN (-1)   // flipped after hardware test (+1 rolled the dot uphill)
#define IMU_SCREEN_Y_SRC  1
#define IMU_SCREEN_Y_SIGN (+1)   // confirmed on hardware
#define IMU_SCREEN_Z_SRC  2
#define IMU_SCREEN_Z_SIGN (+1)   // Z = screen normal confirmed; only |g_z| is used

// ---- Look ---------------------------------------------------------------------------
#define PALETTE_GREEN    0   // P1 phosphor green
#define PALETTE_ORANGE   1   // P3-style amber/orange phosphor (#FFB000 family)
#define PALETTE_COUNT    2
#ifndef PALETTE_DEFAULT
#define PALETTE_DEFAULT  PALETTE_GREEN   // tiltdot / testpattern
#endif

// Sand-mode looks: BOOT long press steps through this table and wraps.
// Switching keeps every grain where it is and only changes colour and
// behaviour (a settled sand pile slumps flat as liquid). The first entry is
// the default at boot.
typedef struct {
    const char *name;
    int palette;
    int material;
} sand_mode_t;

#define SAND_MODE_TABLE {                                  \
    { "green gravel", PALETTE_GREEN, MATERIAL_CLASSIC },   \
    { "orange gravel", PALETTE_ORANGE, MATERIAL_CLASSIC }, \
    { "green sand", PALETTE_GREEN, MATERIAL_SAND },        \
    { "orange sand", PALETTE_ORANGE, MATERIAL_SAND },      \
    { "green liquid", PALETTE_GREEN, MATERIAL_LIQUID },    \
    { "orange liquid", PALETTE_ORANGE, MATERIAL_LIQUID },  \
}

// Mode-name overlay (sand mode): on a BOOT long press and at boot the new
// look's material name ("GRAVEL", "LIQUID") is shown centered in the palette's
// brightest shade, at full strength for OVERLAY_HOLD_MS, then fading out over
// OVERLAY_FADE_MS. 5x7 font scaled by OVERLAY_SCALE (smaller if it would not
// fit), on a black box with OVERLAY_PAD pixels around the text.
#define OVERLAY_HOLD_MS 600
#define OVERLAY_FADE_MS 400
#define OVERLAY_SCALE   4
#define OVERLAY_PAD     6

// Brightness (0..255) of a settled grain.
#define GLOW_BASE        150
// Extra brightness per cell/step of grain speed (clamped to 255).
#define GLOW_SPEED_GAIN  140.0f
// How far the brightest levels blend toward white (0..1), like overdriven phosphor.
#define HOT_WHITE_MIX    0.35f
// Display gamma applied when building the colour table.
#define GLOW_GAMMA       1.6f

// Phosphor persistence: instead of clearing, each frame keeps this fraction of
// the previous glow, so moving sand leaves a short afterglow. 0 disables it.
#define PERSISTENCE_ENABLE 1
#define PERSISTENCE_KEEP   0.60f

// ---- LED / backlight ---------------------------------------------------------
#define BACKLIGHT_PERCENT 90
// RGB LED colour per palette, chosen to look like the screen's colour, not
// copied from it: the WS2812's green is bluer and far brighter than the
// panel's. Gamma-encoded 0xRRGGBB like a screen colour; the driver linearises
// each channel with LED_GAMMA before scaling brightness. Find better values
// with the ledcal environment (README "Matching the LED colour").
#define LED_COLOR_GREEN  0x2DFF00
#define LED_COLOR_ORANGE 0xFF9C00
#define LED_GAMMA         2.2f

// ledcal environment: candidates around the palette's LED_COLOR_*, in the
// gamma-encoded HSV space: hue -/+ LEDCAL_HUE_STEPS x LEDCAL_HUE_STEP_DEG,
// saturation lowered in LEDCAL_SAT_STEPS steps of LEDCAL_SAT_STEP. Shown at
// LED level LEDCAL_LEVEL (0..255, as LED_MAX_LEVEL).
#define LEDCAL_HUE_STEP_DEG 3.0f
#define LEDCAL_HUE_STEPS    5
#define LEDCAL_SAT_STEP     0.1f
#define LEDCAL_SAT_STEPS    4
#define LEDCAL_LEVEL        LED_MAX_LEVEL
// LED brightness (0..255) when the sand is still / at full motion.
#define LED_MIN_LEVEL     6
#define LED_MAX_LEVEL     90
// Total sand motion (sum of grain speeds, cells/step) that maps to LED_MAX_LEVEL.
#define LED_MOTION_FULL   400.0f
// Breathing period of the idle pulse.
#define LED_PULSE_PERIOD_MS 3000

// ---- Idle dimming (battery) ----------------------------------------------------
// After IDLE_DIM_AFTER_S without real activity (a knock, a tilt of more than
// IDLE_WAKE_TILT_DEG from where the board was last moved to, sand still
// moving, a button press) the backlight fades to IDLE_DIM_BACKLIGHT_PERCENT
// and the LED to IDLE_DIM_LED_SCALE of its brightness over IDLE_DIM_FADE_MS.
// Any activity restores full brightness on the next frame. Hand tremor and
// sway (a few degrees) stay inside IDLE_WAKE_TILT_DEG, so holding the board
// still does not keep it bright (tools/sim_idle.c checks this).
#define IDLE_DIM_ENABLE            1
#define IDLE_DIM_AFTER_S           180
#define IDLE_DIM_BACKLIGHT_PERCENT 8
#define IDLE_DIM_LED_SCALE         0.2f
#define IDLE_DIM_FADE_MS           2000
#define IDLE_WAKE_TILT_DEG         15.0f
// Total sand motion (sum of grain speeds, cells/step) that counts as moving.
#define IDLE_WAKE_MOTION           20.0f

// ---- Battery ------------------------------------------------------------------------
// BAT -> R1 200K / R4 100K (1 %) -> BAT_ADC (GPIO1, ADC1 channel 0), so the
// ADC sees a third of the battery voltage. Read once per BAT_SAMPLE_MS on
// core 1 (BAT_ADC_SAMPLES conversions averaged), calibrated by ESP-IDF's
// curve-fitting scheme, then smoothed (BAT_FILTER_S time constant).
// BAT_ADC_SCALE corrects the divider; 1.0072 is the correction Waveshare's own
// demo applies (BAT_Driver.c: / 0.992857). Measure the cell with a meter and
// compare with the banner / stats line to refine it.
#define BAT_SAMPLE_MS      1000
#define BAT_ADC_SAMPLES    16
#define BAT_DIVIDER        3.0f
#define BAT_ADC_SCALE      1.0072f
#define BAT_FILTER_S       60.0f
// The shown percent follows the filtered reading down once it is
// BAT_HYST_PERCENT below, but up only after BAT_RISE_PERCENT (on battery the
// charge only falls; a cell recovers a little when the load drops). The
// 5-bar level needs BAT_HYST_PERCENT past a bar boundary to change.
#define BAT_HYST_PERCENT   2
#define BAT_RISE_PERCENT   10
// Above this the cell is on the charger (or there is no cell and the charger
// output is read): the level is not meaningful, the OSD animates instead.
// The board cannot sense USB or the charger's STAT pin, so this is the only
// hint; a cell fresh off the charger can sit just below it for a while.
// A cell charging from part full stays below it until near the end of the
// charge, so "charging" really means "on USB with the cell (nearly) full, or
// no cell". BAT_CHARGING_HYST_MV keeps it from flickering.
#define BAT_CHARGING_MV      4190
#define BAT_FAST_FILTER_S    5.0f
#define BAT_CHARGING_HYST_MV 30
// Below this the reading is not a battery (no cell and no USB cannot happen
// while running; this catches a broken divider or ADC read).
#define BAT_VALID_MIN_MV   2500

// Battery OSD (BOOT double click): an icon with 5 bars (20 % each) for
// BAT_OSD_MS, the last BAT_OSD_FADE_MS of it fading out. While charging the
// bars climb one by one every BAT_OSD_CHARGE_STEP_MS.
#define BAT_OSD_MS             3000
#define BAT_OSD_FADE_MS        400
#define BAT_OSD_CHARGE_STEP_MS 300

// Low battery: below BAT_LOW_PERCENT (and not charging) the LED blinks red
// instead of the palette pulse; it stops above BAT_LOW_PERCENT +
// BAT_HYST_PERCENT. On for BAT_LOW_BLINK_ON_MS every BAT_LOW_BLINK_MS.
#define BAT_LOW_ENABLE      1
#define BAT_LOW_PERCENT     15
#define BAT_LOW_BLINK_MS    1000
#define BAT_LOW_BLINK_ON_MS 150
#define BAT_LOW_COLOR       0xFF0000

// Low-voltage cutoff: when the cell (60 s filtered, under load) falls below
// BAT_CUTOFF_MV, the empty battery icon blinks for BAT_CUTOFF_WARN_MS, the
// picture collapses and the board sleeps with motion wake off (only BOOT and
// RESET wake it). Every boot checks a settled reading first: below the
// cutoff (plus BAT_CUTOFF_RESUME_MV after a cutoff, since a rested cell reads
// higher than one under load) it shows the warning and sleeps again. Not
// applied while the board appears to be on USB (reading above
// BAT_CHARGING_MV, or a USB host connected to the serial port).
#define BAT_CUTOFF_ENABLE    1
#define BAT_CUTOFF_MV        3300
#define BAT_CUTOFF_RESUME_MV 100
#define BAT_CUTOFF_WARN_MS   2000
#define BAT_CUTOFF_BLINK_MS  400  // icon on half of this, off half

// Drain log: settled reading for the voltage stored at power-off and read at
// wake, BAT_SETTLED_SAMPLES conversions after discarding a few and waiting
// BAT_SETTLE_MS.
#define BAT_SETTLED_SAMPLES  64
#define BAT_SETTLE_MS        20

// ---- Logging ---------------------------------------------------------------------
// Once-a-second statistics line on the console (sand mode). Off in the release
// environment (RELEASE_BUILD); build with -DSTATS_LOG=1 to turn it back on.
#ifndef STATS_LOG
#if defined(RELEASE_BUILD) && RELEASE_BUILD
#define STATS_LOG 0
#else
#define STATS_LOG 1
#endif
#endif

// ---- Button (BOOT) --------------------------------------------------------------------
// Gestures (src/gesture.c, checked by tools/sim_power.c):
//   click         press shorter than BTN_LONG_MS, no second press within
//                 BTN_DOUBLE_MS of the release: respawn (fires when the
//                 window has passed, so it is never the start of a double)
//   double click  two such presses, the second starting within BTN_DOUBLE_MS
//                 of the first release (fires on the second release)
//   long press    released between BTN_LONG_MS and BTN_HOLD_SHOW_MS: next mode
//                 (fires on release, so a hold never changes the mode first)
//   hold          from BTN_HOLD_SHOW_MS a progress bar fills; releasing then
//                 cancels with no action; reaching BTN_POWER_OFF_MS powers off
//                 (fires while still held)
// A click followed by a long press or hold counts only as the long press /
// hold. A press already down at boot (the one that woke the board) is
// ignored until released.
#define BTN_DOUBLE_MS    300
#define BTN_LONG_MS      1000
#define BTN_HOLD_SHOW_MS 1500
#define BTN_POWER_OFF_MS 3000

// ---- Power ------------------------------------------------------------------------------
// Power off = CRT collapse animation, backlight off, LCD controller to sleep,
// LED dark, QMI8658 to wake-on-motion, ESP32-S3 deep sleep. Wakes on a BOOT
// press, on RESET, or on motion (QMI8658 INT2 -> GPIO12). The current mode
// is kept in RTC memory across the sleep (not across RESET or power loss).
//
// Auto-off: no activity (the idle-dimming definition above: a knock, a real
// tilt, sand still moving, a button press) for AUTO_OFF_S powers off.
#define AUTO_OFF_ENABLE 1
#define AUTO_OFF_S      (15 * 60)

// Wake on motion: the QMI8658 runs its accelerometer alone in low-power mode
// and toggles INT2 when the motion it sees exceeds the threshold (datasheet:
// "absolute value in mg", 1 mg per step, 1..255; how it is measured is not
// documented). 150 mg at 11 Hz is meant to ignore light desk bumps and wake
// on a pick-up or a tilt; a starting value, tune on hardware.
#define WOM_ENABLE       1
#define WOM_THRESHOLD_MG 150
// Low-power accelerometer rate (CTRL2 aODR): 0x0C = 128 Hz, 0x0D = 21 Hz,
// 0x0E = 11 Hz, 0x0F = 3 Hz (datasheet: 55 / 42 / 35 / 30 uA).
#define WOM_ODR          0x0E
// Samples ignored after entering wake-on-motion (settling), 0..63.
#define WOM_BLANKING     8
// Motion wake is armed this long after power-off (until then only BOOT and
// RESET wake the board), so setting the board down after a hold-to-off does
// not wake it straight away. The chip wakes briefly on a timer to arm it
// (no display, no LED; about a quarter of a second).
#define WOM_ARM_DELAY_S  10

// LED data line in deep sleep. The board pulls it up to 3V3 through 10 kOhm
// (schematic R21), so holding it low would draw 330 uA for the whole sleep.
// 0: hold it high (no current; the LED latches nothing without a low reset
// pulse), 1: hold it low.
#define SLEEP_LED_DATA_LOW 0

// CRT power animations (ms). Off: the picture squeezes vertically into a
// bright line, the line shrinks to a dot, the dot fades. On: the mirror.
#define CRT_OFF_SQUEEZE_MS 300
#define CRT_OFF_SHRINK_MS  200
#define CRT_OFF_FADE_MS    200
#define CRT_ON_DOT_MS      150
#define CRT_ON_LINE_MS     200
#define CRT_ON_OPEN_MS     300
