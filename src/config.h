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
// angle of repose) or SPREAD (water: flow sideways toward any lower spot, so
// the surface levels perpendicular to gravity).
#define MAT_REST_REPOSE 0
#define MAT_REST_SPREAD 1

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
    int level_rate;         // grains per step moved from high to low columns (0 = off)
    int level_range;        // how many lines across gravity such a move may span
    float level_max_motion; // skip levelling while total motion is above this (splashing)
    // Sleep
    int wake_radius;        // cells around a vacated cell whose sleepers wake (0 = WAKE_RADIUS)
    // Look
    float shade_spread;     // 1 = full palette shade variation, 0 = one shade
} material_t;

#define MATERIAL_CLASSIC 0
#define MATERIAL_WATER   1
#define MATERIAL_COUNT   2

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

// WATER: same engine, no angle of repose. Low bounce and almost no friction
// on the pile so it sloshes; resting surface grains flow toward lower spots
// up to spread_scan cells away; less glass friction than sand, but enough to
// lie still when flat. Shades pulled together for a smoother body.
#define MATERIAL_WATER_PARAMS {                                                     \
    .name = "water",                                                                \
    .gravity_scale = 0.06f, .damping = 0.005f, .elasticity = 0.05f,                 \
    .max_speed = 2.0f, .jitter = 0.01f, .contact_friction = 0.02f,                  \
    .rest_speed = 0.15f, .glass_friction = 0.08f, .rule_token = 0.8f,               \
    .rest_rule = MAT_REST_SPREAD, .rule_prob = 1.0f,                                \
    .repose_angle_deg = 0.0f, .repose_scan = 0,                                     \
    .repose_diag_correction_deg = 0.0f, .repose_dither = 0.0f,                      \
    .spread_scan = 24, .spread_push = 0.8f, .spread_step = 0,                       \
    .level_rate = 8, .level_range = 86, .level_max_motion = 400.0f,                 \
    .wake_radius = 1,                                                               \
    .shade_spread = 0.3f,                                                           \
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
// Water levelling is re-run only when grains moved or gravity turned by more
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
// behaviour (a settled sand pile slumps flat as water). The first entry is
// the default at boot.
typedef struct {
    const char *name;
    int palette;
    int material;
} sand_mode_t;

#define SAND_MODE_TABLE {                                \
    { "green gravel", PALETTE_GREEN, MATERIAL_CLASSIC },   \
    { "orange gravel", PALETTE_ORANGE, MATERIAL_CLASSIC }, \
    { "green water", PALETTE_GREEN, MATERIAL_WATER },      \
    { "orange water", PALETTE_ORANGE, MATERIAL_WATER },    \
}

// Mode-name overlay (sand mode): on a BOOT long press and at boot the new
// look's material name ("GRAVEL", "WATER") is shown centered in the palette's
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
// LED brightness (0..255) when the sand is still / at full motion.
#define LED_MIN_LEVEL     6
#define LED_MAX_LEVEL     90
// Total sand motion (sum of grain speeds, cells/step) that maps to LED_MAX_LEVEL.
#define LED_MOTION_FULL   400.0f
// Breathing period of the idle pulse.
#define LED_PULSE_PERIOD_MS 3000

// ---- Button --------------------------------------------------------------------------
#define BTN_LONG_MS 700
