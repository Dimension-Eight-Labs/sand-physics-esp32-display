// All user-tunable parameters for the demo, in one place.
//
// This header is included by the hardware-free modules (sim, palette, render),
// so it must only contain plain macros.
#pragma once

#include "board.h"

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

// ---- Physics (units: cells and simulation steps) -----------------------------
// Acceleration in cells/step^2 per 1 g of in-plane gravity.
#define GRAVITY_SCALE     0.06f
// Fraction of velocity lost per step while moving (air drag).
#define DAMPING           0.01f
// Velocity kept (and reversed) on a blocked axis; 0 = dead stop, 1 = perfect bounce.
#define ELASTICITY        0.30f
// Speed limit in cells/step. Moves above 1 cell/step are sub-stepped.
#define MAX_SPEED         2.0f
// Random velocity jitter (cells/step, uniform +/-) added to moving grains.
#define JITTER            0.01f
// Tangential velocity lost per step while a moving grain is in contact with
// the pile/wall below it (sliding friction).
#define CONTACT_FRICTION  0.30f
// A supported grain slower than this (cells/step) is "resting": its velocity is
// zeroed and only the repose rule can move it. Must exceed GRAVITY_SCALE.
#define REST_SPEED        0.15f
// Coulomb friction of the sand against the glass, proportional to how hard
// gravity presses it there: friction = BACK_FRICTION * |g_z| (in g). It slows
// moving grains, holds still ones unless the in-plane pull is larger, and makes
// piles stand steeper when the board is leaned back. Upright (g_z = 0) there is
// none; the sand only moves while the screen is more than ~atan(BACK_FRICTION)
// = 8.5° from flat (lean back < ~81.5° from vertical).
#define BACK_FRICTION     0.15f
// In-plane gravity below this (g, after back friction) is treated as zero.
#define GRAVITY_DEADZONE  0.03f

// ---- Angle of repose -------------------------------------------------------
// Piles steeper than this (degrees, relative to true gravity) avalanche.
#define REPOSE_ANGLE_DEG  33.0f
// Probability per step that a resting surface grain evaluates the repose rule.
// Lower = slower, more granular avalanches; does not change the final angle much.
#define REPOSE_PROB       0.35f
// How many cells sideways the slope is measured over (1..6). 1 only gives 45°
// piles; 3-4 lets the grid represent ~30-37° slopes.
#define REPOSE_SCAN       4
// When gravity is closest to a grid diagonal, slopes are measured along
// diagonals and come out ~1 cell short; lowering the threshold by this many
// degrees keeps the settled angle consistent (measured with tools/sim_host.c).
#define REPOSE_DIAG_CORRECTION_DEG 4.0f
// Random +/- fraction applied to the slide threshold on every check. Lets the
// settled angle land between the slopes the grid can represent (matters when
// glass friction raises the angle while leaned back). 0.25 already flattens
// upright piles noticeably.
#define REPOSE_DITHER     0.15f

// ---- Performance: sleeping grains -----------------------------------------------
// A grain that hasn't moved for SLEEP_STEPS steps is skipped until something
// can change its situation: a grain leaving a cell within WAKE_RADIUS of it,
// gravity moving by more than WAKE_GRAVITY_DELTA_G (vector distance, g), a
// shake or a respawn. These are performance settings: with 64 / 2 the host
// tests (tools/sim_host.c) match running without sleep to within ~0.5°;
// radius 1 left piles up to 4.5° too steep (sleepers miss far-away changes).
#ifndef SLEEP_STEPS
#define SLEEP_STEPS          64
#endif
#ifndef WAKE_RADIUS
#define WAKE_RADIUS          2
#endif
#ifndef WAKE_GRAVITY_DELTA_G
#define WAKE_GRAVITY_DELTA_G 0.02f
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
#define PALETTE_P1_GREEN 0
#define PALETTE_P3_AMBER 1
#define PALETTE_ORANGE   2   // gas-plasma orange
#define PALETTE_COUNT    3   // BOOT long press cycles green -> amber -> orange
#ifndef PALETTE_DEFAULT
#define PALETTE_DEFAULT  PALETTE_P1_GREEN
#endif

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
