// Application: bring-up modes and the falling-sand main loop.
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery.h"
#include "button.h"
#include "config.h"
#include "display.h"
#include "idle.h"
#include "imu.h"
#include "led.h"
#include "ledcolor.h"
#include "material.h"
#include "palette.h"
#include "power.h"
#include "render.h"
#include "sim.h"

static const char *TAG = "app";

#define APP_TASK_STACK 6144
#define APP_TASK_PRIO  4
#define APP_TASK_CORE  1

#define FRAME_US   (1000000 / TARGET_FPS)
// Slack for frame-time jitter (1 ms ticks) when the sim times its publish.
// 3 ms gave 99-100 % new frames in a timing model with 0.5-9.5 ms steps.
#define SNAP_MARGIN_US 3000
#define SIM_DT_US  (1000000 / SIM_HZ)

#if APP_MODE == MODE_TEST_PATTERN
#define APP_MODE_NAME "testpattern"
#elif APP_MODE == MODE_TILT_DOT
#define APP_MODE_NAME "tiltdot"
#elif APP_MODE == MODE_SAND
#define APP_MODE_NAME "sand"
#elif APP_MODE == MODE_LED_CAL
#define APP_MODE_NAME "ledcal"
#endif

#if APP_MODE == MODE_SAND
#define SIM_TASK_STACK 4096
#define SIM_TASK_PRIO  4   // below the IMU task (5) on the same core
#define SIM_TASK_CORE  0
static char s_mem_info[160]; // where the sand memory went, for the banner
static const char *s_mode_name = "";
static bool s_mode_restored; // woke into the mode saved at power-off
#endif

// Init results, reported in the banner.
static esp_err_t s_imu_err = ESP_ERR_NOT_FINISHED;
static esp_err_t s_led_err = ESP_ERR_NOT_FINISHED;
static esp_err_t s_btn_err = ESP_ERR_NOT_FINISHED;
static esp_err_t s_bat_err = ESP_ERR_NOT_FINISHED;

// "3.95 V, 70 %, 4 of 5 bars" / "4.21 V, on USB or charging" / an error.
static void battery_text(char *buf, size_t n)
{
    batt_t b;
    battery_get(&b);
    if (s_bat_err != ESP_OK) {
        snprintf(buf, n, "not available (%s)", esp_err_to_name(s_bat_err));
    } else if (!b.valid) {
        snprintf(buf, n, "no reading");
    } else if (b.charging) {
        snprintf(buf, n, "%.2f V, on USB or charging (level not meaningful)", (double)(b.mv / 1000.0f));
    } else {
        snprintf(buf, n, "%.2f V, %d %%, %d of 5 bars%s", (double)(b.mv / 1000.0f), b.percent, b.bars,
                 b.low ? ", LOW" : "");
    }
}

// Drain log: what happened since the last power-off, from the voltage then
// and the settled reading at this boot.
static void drain_text(char *buf, size_t n)
{
    static const char *const WHY[] = { "BOOT held", "auto-off", "battery cutoff" };
    const power_last_off_t off = power_last_off();
    const float now_mv = battery_boot_mv();
    if (!off.valid) {
        snprintf(buf, n, "none (not a wake from power-off)");
        return;
    }
    const int h = (int)(off.slept_s / 3600.0), m = (int)(off.slept_s / 60.0) % 60;
    if (off.mv_at_off <= 0.0f || now_mv <= 0.0f) {
        snprintf(buf, n, "%s, slept %d h %02d min, no voltage reading", WHY[off.reason], h, m);
        return;
    }
    const int len = snprintf(buf, n, "%s, slept %d h %02d min, %.3f V -> %.3f V, ", WHY[off.reason], h, m,
                             (double)(off.mv_at_off / 1000.0f), (double)(now_mv / 1000.0f));
    if (len < 0 || (size_t)len >= n) {
        return;
    }
    if (off.slept_s < 3600.0) {
        snprintf(buf + len, n - (size_t)len, "too short to estimate a drain (under 1 h)");
    } else {
        snprintf(buf + len, n - (size_t)len, "drain %.1f mV/day",
                 (double)batt_drain_mv_per_day(off.mv_at_off, now_mv, off.slept_s));
    }
}

static void print_banner(void)
{
    imu_info_t imu = imu_get_info();
    printf("\n");
    printf("==================== phosphor sand ====================\n");
    printf(" mode      : %s (APP_MODE=%d)\n", APP_MODE_NAME, APP_MODE);
    printf(" firmware  : %s, built %s %s\n", esp_app_get_description()->version, __DATE__, __TIME__);
#if defined(RELEASE_BUILD) && RELEASE_BUILD
    printf(" build     : release (log level warning, stats log %s)\n", STATS_LOG ? "on" : "off");
#else
    printf(" build     : development (stats log %s)\n", STATS_LOG ? "on" : "off");
#endif
    printf(" ESP-IDF   : %s\n", esp_get_idf_version());
    printf(" started by: %s\n", power_boot_reason());
    printf(" display   : %dx%d @ %d MHz, offset (%d,%d), invert %d, BGR %d, swap bytes %d\n",
           SCREEN_W, SCREEN_H, BOARD_LCD_SPI_HZ / 1000000, BOARD_LCD_OFFSET_X, BOARD_LCD_OFFSET_Y,
           BOARD_LCD_INVERT, BOARD_LCD_BGR, BOARD_LCD_SWAP_BYTES);
    if (imu.found) {
        printf(" IMU       : QMI8658 at 0x%02X, WHO_AM_I 0x%02X (%s)%s%s\n", imu.addr, imu.who_am_i,
               imu.who_am_i == 0x05 ? "ok" : "unexpected, want 0x05",
               s_imu_err == ESP_OK ? "" : ", init error: ", s_imu_err == ESP_OK ? "" : esp_err_to_name(s_imu_err));
    } else {
        printf(" IMU       : not found (%s)\n", esp_err_to_name(s_imu_err));
    }
    printf(" RGB LED   : %s, button: %s\n", esp_err_to_name(s_led_err), esp_err_to_name(s_btn_err));
    char bat[96];
    battery_text(bat, sizeof(bat));
    printf(" battery   : %s; ADC calibration %s\n", bat, battery_calibration());
    char drain[160];
    drain_text(drain, sizeof(drain));
    printf(" last off  : %s\n", drain);
#if APP_MODE == MODE_SAND
    printf(" sand      : %s\n", s_mem_info);
    printf(" look      : %s%s (BOOT long press cycles the sand-mode table)\n", s_mode_name,
           s_mode_restored ? ", restored after power-off" : "");
    printf(" power     : hold BOOT %d s = off; auto-off after %d min idle%s; wake on BOOT, RESET%s\n",
           BTN_POWER_OFF_MS / 1000, AUTO_OFF_S / 60, AUTO_OFF_ENABLE ? "" : " (disabled)",
           WOM_ENABLE ? ", motion" : "");
    printf(" cores     : simulation on core %d, rendering on core %d\n", SIM_TASK_CORE, APP_TASK_CORE);
#endif
    printf(" free heap : internal %u bytes, PSRAM %u bytes\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("=======================================================\n");
}

// Call regularly: re-prints the banner when a USB host (serial monitor)
// attaches, since boot output is usually gone before the port is opened.
static void console_tick(void)
{
    static bool was_connected = true; // banner already printed at boot
    bool connected = usb_serial_jtag_is_connected();
    if (connected && !was_connected) {
        print_banner();
    }
    was_connected = connected;
}

#if APP_MODE == MODE_TILT_DOT || APP_MODE == MODE_SAND
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Sleep until `*next` (µs), keeping a fixed cadence; resync if we fell behind.
static void pace_frame(int64_t *next)
{
    *next += FRAME_US;
    int64_t wait_us = *next - esp_timer_get_time();
    if (wait_us >= 1000) {
        vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
    } else {
        if (wait_us < -FRAME_US) {
            *next = esp_timer_get_time();
        }
        vTaskDelay(1); // always yield so lower-priority tasks get a tick
    }
}
#endif

// ---- MODE_TEST_PATTERN -------------------------------------------------------

#if APP_MODE == MODE_TEST_PATTERN
static void print_expected_pattern(void)
{
    ESP_LOGI(TAG, "Test pattern. Expect, top to bottom: white, yellow, cyan, green, magenta, red, blue, black");
    ESP_LOGI(TAG, "with a black-to-white grey ramp; red square top-left, green top-right, blue bottom-left;");
    ESP_LOGI(TAG, "a 1-px white border visible on all four edges; RGB LED pulsing red.");
}

static void run_test_pattern(void)
{
    render_test_pattern(display_framebuffer());
    display_push_async();
    display_wait();
    print_expected_pattern();

    // Pure red on the LED checks its colour order too.
    uint32_t seconds = 0;
    for (int tick = 0;; tick++) {
        led_update(0xFF0000, 0.5f, 1.0f, (uint32_t)(esp_timer_get_time() / 1000));
        if (tick % 20 == 0) {
            console_tick();
            printf("[testpattern] alive, uptime %" PRIu32 " s\n", seconds++);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

// ---- MODE_TILT_DOT ---------------------------------------------------------------

#if APP_MODE == MODE_TILT_DOT
static void run_tilt_dot(void)
{
    const int r = 5;
    float x = SCREEN_W / 2.0f, y = SCREEN_H / 2.0f, vx = 0.0f, vy = 0.0f;
    uint16_t *fb = display_framebuffer();
    const palette_t *pal = palette_get(PALETTE_DEFAULT);
    uint16_t dot = render_color(pal->shade[0]);
    uint16_t edge = render_color(0x202020);
    int64_t next = esp_timer_get_time();
    uint32_t last_log = 0;

    ESP_LOGI(TAG, "Tilt dot: the dot must roll toward the lowest edge of the screen.");
    ESP_LOGI(TAG, "in-plane angle: 0 = toward bottom edge, +90 = right edge, -90 = left edge, 180 = top edge");
    for (;;) {
        imu_state_t imu;
        imu_get(&imu, true);

        // Same gravity model as the sand: the in-plane projection pulls the dot
        // (not normalised), and Coulomb friction against the glass grows with
        // the screen-normal component.
        const float accel = 0.4f; // pixels/frame^2 per g
        float g_xy = sqrtf(imu.gx * imu.gx + imu.gy * imu.gy);
        float glass = material_get(MATERIAL_CLASSIC)->glass_friction * fabsf(imu.gz);
        vx = (vx + imu.gx * accel) * 0.98f;
        vy = (vy + imu.gy * accel) * 0.98f;
        float sp = sqrtf(vx * vx + vy * vy);
        float f = glass * accel;
        if (sp <= f) {
            vx = vy = 0.0f;
        } else {
            vx *= 1.0f - f / sp;
            vy *= 1.0f - f / sp;
        }
        x += vx;
        y += vy;
        if (x < r) { x = r; vx = -vx * 0.4f; }
        if (x > SCREEN_W - 1 - r) { x = SCREEN_W - 1 - r; vx = -vx * 0.4f; }
        if (y < r) { y = r; vy = -vy * 0.4f; }
        if (y > SCREEN_H - 1 - r) { y = SCREEN_H - 1 - r; vy = -vy * 0.4f; }

        display_wait();
        render_fill(fb, 0);
        render_rect(fb, 0, 0, SCREEN_W, 1, edge);
        render_rect(fb, 0, SCREEN_H - 1, SCREEN_W, 1, edge);
        render_rect(fb, 0, 0, 1, SCREEN_H, edge);
        render_rect(fb, SCREEN_W - 1, 0, 1, SCREEN_H, edge);
        render_rect(fb, (int)x - r, (int)y - r, 2 * r + 1, 2 * r + 1, dot);
        display_push_async();
        led_update(pal->led, sp / 8.0f, 1.0f, now_ms());

        uint32_t t = now_ms();
        if (t - last_log >= 100) {
            last_log = t;
            console_tick();
            float angle = atan2f(imu.gx, imu.gy) * (180.0f / 3.14159265f);
            printf("raw ax=%+.3f ay=%+.3f az=%+.3f | screen gx=%+.3f gy=%+.3f gz=%+.3f | "
                   "in-plane %+6.1f deg %.3f g | glass friction %.3f g | net %+.3f g%s%s\n",
                   (double)imu.raw[0], (double)imu.raw[1], (double)imu.raw[2],
                   (double)imu.gx, (double)imu.gy, (double)imu.gz, (double)angle, (double)g_xy,
                   (double)glass, (double)(g_xy - glass), g_xy - glass < GRAVITY_DEADZONE ? " (held by friction)" : "",
                   imu.ok ? "" : "  (no IMU data)");
        }
        pace_frame(&next);
    }
}
#endif

// ---- MODE_SAND ----------------------------------------------------------------------
//
// Two tasks:
//  - sim task, core 0: owns the simulation and runs fixed SIM_HZ steps on its
//    own clock, so physics stays real time no matter how fast frames are
//    drawn. After stepping it packs the grains into a per-cell snapshot.
//  - render task, core 1: draws the newest snapshot, pushes it over SPI DMA,
//    drives the LED and handles the button. If it falls behind it simply
//    shows fewer frames; it never holds up the physics.
//
// Hand-over is a triple buffer of snapshots: the sim writes `back`, the
// renderer reads `front`, and `ready` is the newest finished one. Publishing
// swaps back<->ready, taking swaps ready<->front; both are a few-instruction
// pointer swap under a spinlock, so neither task ever waits for the other,
// and neither touches a buffer the other is using. No other sim state is
// shared: the button reaches the sim through an atomic flag, and statistics
// travel under their own spinlock.

#if APP_MODE == MODE_SAND

typedef struct {
    uint8_t *buf[3];
    int front, ready, back;
    bool fresh; // `ready` holds a snapshot the renderer hasn't taken yet
    int64_t last_take_us; // when the renderer last looked for a snapshot
} snapshots_t;

typedef struct {
    uint32_t steps;
    int64_t step_us;     // total time in sim_step
    int64_t max_step_us;
    int64_t snap_us;     // total time building snapshots
    uint32_t snaps;
    int64_t lost_us;     // sim time dropped because the sim itself couldn't keep up
    int awake;
    float motion;
} sim_stats_t;

static sim_t s_sim;
static snapshots_t s_snap;
static portMUX_TYPE s_snap_lock = portMUX_INITIALIZER_UNLOCKED;
static sim_stats_t s_stats;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_respawn_req; // accessed with __atomic builtins
static int64_t s_active_us; // last real activity seen by the sim task (idle dimming), under s_stats_lock
// Sand-mode looks (palette + material); the render task picks one, the sim
// task applies its material. Both accessed with __atomic builtins.
static const sand_mode_t MODES[] = SAND_MODE_TABLE;
#define MODE_COUNT ((int)(sizeof(MODES) / sizeof(MODES[0])))
static int s_mode_req;

static void apply_look(int mode)
{
    const sand_mode_t *md = &MODES[mode];
    render_set_look(md->palette, material_get(md->material)->shade_spread);
    s_mode_name = md->name;
}

static void *alloc_prefer_internal(size_t bytes, bool *internal)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    *internal = p != NULL;
    if (!p) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return p;
}

// Allocate everything before the banner so it can report where it went.
static esp_err_t sand_init(void)
{
    const int n = (int)(FILL_FRACTION * (float)(GRID_W * GRID_H));
    const size_t sim_bytes = sim_mem_bytes(GRID_W, GRID_H, n);
    const size_t snap_bytes = (size_t)GRID_W * GRID_H;

    // The simulation is the hot data (random access every step): internal RAM.
    // Snapshots are written once per frame and read sequentially, which the
    // PSRAM cache handles well (one cache shared by both cores, so coherent),
    // so they go to PSRAM to leave internal RAM for the sim and task stacks.
    bool sim_internal, snap_internal = false;
    void *mem = alloc_prefer_internal(sim_bytes, &sim_internal);
    if (!mem) {
        ESP_LOGE(TAG, "cannot allocate %u bytes for the simulation", (unsigned)sim_bytes);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < 3; i++) {
        s_snap.buf[i] = heap_caps_malloc(snap_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_snap.buf[i]) {
            s_snap.buf[i] = heap_caps_malloc(snap_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            snap_internal = true;
        }
        if (!s_snap.buf[i]) {
            return ESP_ERR_NO_MEM;
        }
        memset(s_snap.buf[i], 0, snap_bytes);
    }
    s_snap.front = 0;
    s_snap.ready = 1;
    s_snap.back = 2;

    snprintf(s_mem_info, sizeof(s_mem_info),
             "%d grains, sim %u B in %s, snapshots 3 x %u B in %s",
             n, (unsigned)sim_bytes, sim_internal ? "internal RAM" : "PSRAM (SLOW!)",
             (unsigned)snap_bytes, snap_internal ? "internal RAM" : "PSRAM");
    if (!sim_internal) {
        ESP_LOGW(TAG, "simulation memory is in PSRAM; expect much slower steps");
    }

    // A wake from power-off resumes the saved mode; anything else starts in
    // the first. The sand always respawns.
    int mode = power_saved_mode();
    s_mode_restored = mode >= 0 && mode < MODE_COUNT;
    if (!s_mode_restored) {
        mode = 0;
    }
    s_mode_req = mode;
    sim_init(&s_sim, mem, GRID_W, GRID_H, n, esp_random());
    sim_set_material(&s_sim, material_get(MODES[mode].material));
    sim_spawn(&s_sim, SPAWN_REGION_FRACTION, PALETTE_SHADES);
    render_init(MODES[mode].palette);
    apply_look(mode);
    return ESP_OK;
}

static void sim_task(void *arg)
{
    (void)arg;
    int64_t last = esp_timer_get_time();
    int64_t acc_us = 0;
    int mode = __atomic_load_n(&s_mode_req, __ATOMIC_ACQUIRE);
    idle_t idle;
    idle_init(&idle, last);

    for (;;) {
        int64_t now = esp_timer_get_time();
        acc_us += now - last;
        last = now;

        // Material follows the selected look; grains stay where they are.
        int want_mode = __atomic_load_n(&s_mode_req, __ATOMIC_ACQUIRE);
        if (want_mode != mode) {
            if (MODES[want_mode].material != MODES[mode].material) {
                sim_set_material(&s_sim, material_get(MODES[want_mode].material));
            }
            mode = want_mode;
        }

        // Only if a single step costs more than SIM_DT_US can the backlog grow
        // without bound; then cap it (physics slows down) and report it.
        int64_t lost = 0;
        if (acc_us > SIM_MAX_BACKLOG_MS * 1000) {
            lost = acc_us - SIM_MAX_BACKLOG_MS * 1000;
            acc_us = SIM_MAX_BACKLOG_MS * 1000;
        }

        if (acc_us >= SIM_DT_US) {
            imu_state_t imu;
            imu_get(&imu, true);
            sim_set_gravity(&s_sim, imu.gx, imu.gy, imu.gz);
            if (imu.shake > 0.0f) {
                sim_shake(&s_sim, imu.shake_dx, imu.shake_dy, SHAKE_STRENGTH * imu.shake);
            }
            const float g[3] = { imu.gx, imu.gy, imu.gz };
            if (idle_update(&idle, g, imu.shake > 0.0f, s_sim.motion, now)) {
                portENTER_CRITICAL(&s_stats_lock);
                s_active_us = now;
                portEXIT_CRITICAL(&s_stats_lock);
            }
            if (__atomic_exchange_n(&s_respawn_req, false, __ATOMIC_ACQ_REL)) {
                sim_spawn(&s_sim, SPAWN_REGION_FRACTION, PALETTE_SHADES);
            }

            uint32_t steps = 0, snaps = 0;
            int64_t step_us = 0, max_us = 0, snap_us = 0;
            while (acc_us >= SIM_DT_US) {
                int64_t t0 = esp_timer_get_time();
                sim_step(&s_sim);
                int64_t dt = esp_timer_get_time() - t0;
                step_us += dt;
                max_us = dt > max_us ? dt : max_us;
                acc_us -= SIM_DT_US;
                steps++;

                // Hand the newest state to the next frame. Publish after this
                // step if the renderer's next frame comes before another step
                // can finish (so the frame gets the latest state), or if the
                // renderer took the last snapshot and its frame is already due
                // (safety net). Normally that is one snapshot per frame, built
                // from the last step before the frame. (Publishing only at the
                // end of a batch left frames without new data whenever long
                // steps made the sim run two steps back to back.)
                bool want;
                int64_t now_us = esp_timer_get_time();
                int64_t next_step_done = now_us + (dt > SIM_DT_US ? dt : SIM_DT_US);
                portENTER_CRITICAL(&s_snap_lock);
                int64_t due = s_snap.last_take_us + FRAME_US;
                want = next_step_done > due - SNAP_MARGIN_US || (!s_snap.fresh && now_us >= due - SNAP_MARGIN_US);
                portEXIT_CRITICAL(&s_snap_lock);
                if (want) {
                    int64_t t1 = esp_timer_get_time();
                    render_snapshot(&s_sim, s_snap.buf[s_snap.back]);
                    snap_us += esp_timer_get_time() - t1;
                    snaps++;
                    portENTER_CRITICAL(&s_snap_lock);
                    int t = s_snap.back;
                    s_snap.back = s_snap.ready;
                    s_snap.ready = t;
                    s_snap.fresh = true;
                    portEXIT_CRITICAL(&s_snap_lock);
                }
            }

            portENTER_CRITICAL(&s_stats_lock);
            s_stats.steps += steps;
            s_stats.step_us += step_us;
            s_stats.max_step_us = max_us > s_stats.max_step_us ? max_us : s_stats.max_step_us;
            s_stats.snap_us += snap_us;
            s_stats.snaps += snaps;
            s_stats.lost_us += lost;
            s_stats.awake = s_sim.awake;
            s_stats.motion = s_sim.motion;
            portEXIT_CRITICAL(&s_stats_lock);
        }

        // Sleep until the next step is due. Always at least one tick, which
        // also lets this core's idle task (and its watchdog) run.
        int64_t wait_us = SIM_DT_US - acc_us;
        vTaskDelay(wait_us >= 2000 ? pdMS_TO_TICKS(wait_us / 1000) : 1);
    }
}

// Take the newest snapshot, if there is one, and tell the sim when this
// frame took it (it times its next publish for the next frame).
static bool take_snapshot(void)
{
    int64_t take_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_snap_lock);
    s_snap.last_take_us = take_us;
    bool got = s_snap.fresh;
    if (got) {
        int t = s_snap.front;
        s_snap.front = s_snap.ready;
        s_snap.ready = t;
        s_snap.fresh = false;
    }
    portEXIT_CRITICAL(&s_snap_lock);
    return got;
}

// CRT warm-up: a dot, a line, then the picture opens. The simulation already
// runs, so the freshly spawned sand is falling as the screen opens.
static void play_power_on(uint16_t *fb)
{
    const int64_t t0 = esp_timer_get_time();
    int64_t next = t0;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        float open, width, k;
        const bool more = render_crt_on_at((uint32_t)((now - t0) / 1000), &open, &width, &k);
        if (!more) {
            return;
        }
        take_snapshot();
        display_wait();
        render_merge(s_snap.buf[s_snap.front]);
        render_crt(fb, open, width, k);
        display_push_async();
        led_update(palette_get(render_palette())->led, 0.0f, open > 0.0f ? 1.0f : k, now_ms());
        pace_frame(&next);
    }
}

// CRT collapse from the picture on screen, the LED fading with the dot.
static void play_power_off(uint16_t *fb)
{
    const int64_t t0 = esp_timer_get_time();
    int64_t next = t0;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        float open, width, k;
        const bool more = render_crt_off_at((uint32_t)((now - t0) / 1000), &open, &width, &k);
        display_wait();
        render_crt(fb, open, width, k);
        display_push_async();
        led_update(palette_get(render_palette())->led, 0.0f, open > 0.0f ? 1.0f : k, now_ms());
        if (!more) {
            display_wait();
            return;
        }
        pace_frame(&next);
    }
}

// Plausibly on USB: the reading looks like the charger, or a USB host is
// talking to the serial port. The cutoff is not applied then.
static bool on_usb(const batt_t *b)
{
    return b->charging || usb_serial_jtag_is_connected();
}

// Low-voltage warning: the empty battery icon blinking (with the LED blinking
// red) for BAT_CUTOFF_WARN_MS, over the running sand or over black.
static void play_cutoff_warning(uint16_t *fb, bool with_sand)
{
    const uint32_t c = palette_brightest(palette_get(render_palette()));
    const int64_t t0 = esp_timer_get_time();
    int64_t next = t0;
    for (;;) {
        const int64_t ms = (esp_timer_get_time() - t0) / 1000;
        if (ms >= BAT_CUTOFF_WARN_MS) {
            return;
        }
        const bool on = ms % BAT_CUTOFF_BLINK_MS < BAT_CUTOFF_BLINK_MS / 2;
        if (with_sand) {
            take_snapshot();
        }
        display_wait();
        if (with_sand) {
            render_frame(s_snap.buf[s_snap.front], fb);
        } else {
            render_fill(fb, 0);
        }
        render_battery(fb, 0, c, on ? 1.0f : 0.25f);
        display_push_async();
        led_show(BAT_LOW_COLOR, on ? LED_MAX_LEVEL : 0);
        pace_frame(&next);
    }
}

// Boot check: below the cutoff (a settled reading taken before the backlight
// came on), warn and go back to sleep without starting the sand.
static void boot_cutoff_check(uint16_t *fb)
{
    const float mv = battery_boot_mv();
    if (mv <= 0.0f || !batt_below_cutoff(mv, power_after_cutoff())) {
        return;
    }
    batt_t b;
    battery_get(&b);
    printf("battery %.3f V is below the cutoff (%.3f V%s)\n", (double)(mv / 1000.0f),
           (double)((float)(BAT_CUTOFF_MV + (power_after_cutoff() ? BAT_CUTOFF_RESUME_MV : 0)) / 1000.0f),
           power_after_cutoff() ? " to resume after a cutoff" : "");
    play_cutoff_warning(fb, false);
    // Checked after the warning: a USB host needs a moment to enumerate.
    if (on_usb(&b)) {
        printf("on USB: cutoff not applied\n");
        return;
    }
    play_power_off(fb);
    power_off(__atomic_load_n(&s_mode_req, __ATOMIC_ACQUIRE), s_imu_err == ESP_OK, POWER_OFF_CUTOFF);
}

static void run_sand(void)
{
    uint16_t *fb = display_framebuffer();
    boot_cutoff_check(fb);

#if STATS_LOG
    // A release build logs warnings only; the statistics line is info.
    if (esp_log_level_get(TAG) < ESP_LOG_INFO) {
        esp_log_level_set(TAG, ESP_LOG_INFO);
    }
#endif

    xTaskCreatePinnedToCore(sim_task, "sim", SIM_TASK_STACK, NULL, SIM_TASK_PRIO, NULL, SIM_TASK_CORE);
    play_power_on(fb);

    int64_t next = esp_timer_get_time();
    int64_t stat_t0 = next;
    int frames = 0, new_frames = 0;
    int64_t render_us = 0, wait_us = 0;

    // Mode-name overlay: shown after the power-on animation and after every
    // look change.
    int64_t overlay_t0 = esp_timer_get_time();
    int overlay_mode = __atomic_load_n(&s_mode_req, __ATOMIC_ACQUIRE);

    // Battery OSD (double click): start time, -1 = not shown.
    int64_t osd_t0 = -1;
    power_off_reason_t off_reason = POWER_OFF_BUTTON;

    // Idle dimming: the button counts as activity too.
    int64_t button_us = overlay_t0;
    int backlight = BACKLIGHT_PERCENT;

    for (;;) {
        uint32_t ev = button_take_events();
        const float hold = button_hold_progress();
        if (ev || hold >= 0.0f) {
            button_us = esp_timer_get_time();
        }
        if (ev & GESTURE_POWER_OFF) {
            ESP_LOGI(TAG, "power off (BOOT held)");
            off_reason = POWER_OFF_BUTTON;
            break;
        }
        if (ev & GESTURE_CLICK) {
            __atomic_store_n(&s_respawn_req, true, __ATOMIC_RELEASE);
            render_clear();
            ESP_LOGI(TAG, "respawn");
        }
        if (ev & GESTURE_DOUBLE) {
            osd_t0 = esp_timer_get_time();
            char bat[96];
            battery_text(bat, sizeof(bat));
            printf("battery: %s\n", bat);
        }
        if (ev & GESTURE_LONG) {
            int mode = (__atomic_load_n(&s_mode_req, __ATOMIC_ACQUIRE) + 1) % MODE_COUNT;
            apply_look(mode);
            __atomic_store_n(&s_mode_req, mode, __ATOMIC_RELEASE);
            ESP_LOGI(TAG, "mode: %s (palette %s, material %s)", MODES[mode].name,
                     palette_get(MODES[mode].palette)->name, material_get(MODES[mode].material)->name);
            overlay_t0 = esp_timer_get_time();
            overlay_mode = mode;
        }

        new_frames += take_snapshot();

        int64_t t0 = esp_timer_get_time();
        display_wait();
        int64_t t1 = esp_timer_get_time();
        render_frame(s_snap.buf[s_snap.front], fb);
        int64_t shown_ms = (t1 - overlay_t0) / 1000;
        if (shown_ms < OVERLAY_HOLD_MS + OVERLAY_FADE_MS) {
            float alpha = shown_ms < OVERLAY_HOLD_MS
                              ? 1.0f
                              : 1.0f - (float)(shown_ms - OVERLAY_HOLD_MS) / (float)OVERLAY_FADE_MS;
            const sand_mode_t *md = &MODES[overlay_mode];
            render_overlay(fb, material_get(md->material)->name, palette_brightest(palette_get(md->palette)),
                           alpha);
        }
        batt_t bat;
        battery_get(&bat);
        if (osd_t0 >= 0) {
            const int64_t osd_ms = (t1 - osd_t0) / 1000;
            if (osd_ms >= BAT_OSD_MS) {
                osd_t0 = -1;
            } else {
                const float alpha = osd_ms < BAT_OSD_MS - BAT_OSD_FADE_MS
                                        ? 1.0f
                                        : (float)(BAT_OSD_MS - osd_ms) / (float)BAT_OSD_FADE_MS;
                // On USB / charging the level means nothing: the bars climb.
                const int bars = bat.charging ? (int)((osd_ms / BAT_OSD_CHARGE_STEP_MS) % 6)
                                 : bat.valid  ? bat.bars
                                              : 0;
                render_battery(fb, bars, palette_brightest(palette_get(render_palette())), alpha);
            }
        }
        if (hold >= 0.0f) {
            // Past the long press: keep holding to power off, let go to cancel.
            const uint32_t c = palette_brightest(palette_get(render_palette()));
            render_overlay(fb, "OFF", c, 1.0f);
            render_progress(fb, hold, c);
        }
        display_push_async();
        int64_t t2 = esp_timer_get_time();

        portENTER_CRITICAL(&s_stats_lock);
        float motion = s_stats.motion;
        int64_t active_us = s_active_us > button_us ? s_active_us : button_us;
        portEXIT_CRITICAL(&s_stats_lock);

        // Idle dimming: fade the backlight and LED after a while without
        // activity; back to full on the first frame after any.
        const float dim = idle_dim_amount(t2 - active_us);
        const int want_bl = BACKLIGHT_PERCENT + (int)lroundf((float)(IDLE_DIM_BACKLIGHT_PERCENT - BACKLIGHT_PERCENT) * dim);
        if (want_bl != backlight) {
            backlight = want_bl;
            display_set_backlight(backlight);
        }
        // The LED dims by brightness, not by scaling its (gamma-encoded) colour.
        const float led_k = 1.0f - (1.0f - IDLE_DIM_LED_SCALE) * dim;
        if (bat.low) {
            // Low battery: a short red blink instead of the palette pulse.
            const bool on = now_ms() % BAT_LOW_BLINK_MS < BAT_LOW_BLINK_ON_MS;
            led_show(BAT_LOW_COLOR, on ? (int)lroundf((float)LED_MAX_LEVEL * led_k) : 0);
        } else {
            led_update(palette_get(render_palette())->led, motion / LED_MOTION_FULL, led_k, now_ms());
        }
        if (idle_auto_off(t2 - active_us)) {
            ESP_LOGI(TAG, "auto-off after %d s without activity", AUTO_OFF_S);
            off_reason = POWER_OFF_AUTO;
            break;
        }
        // Low-voltage cutoff, on the filtered reading once it has settled.
        if (bat.valid && bat.n >= 10 && batt_below_cutoff(bat.mv, false) && !on_usb(&bat)) {
            printf("battery %.3f V below the cutoff %.3f V: powering off\n", (double)(bat.mv / 1000.0f),
                   (double)BAT_CUTOFF_MV / 1000.0);
            off_reason = POWER_OFF_CUTOFF;
            break;
        }

        frames++;
        wait_us += t1 - t0;
        render_us += t2 - t1;
        if (t2 - stat_t0 >= 1000000) {
            sim_stats_t st;
            portENTER_CRITICAL(&s_stats_lock);
            st = s_stats;
            s_stats = (sim_stats_t){ .awake = st.awake, .motion = st.motion };
            portEXIT_CRITICAL(&s_stats_lock);
#if STATS_LOG
            float secs = (float)(t2 - stat_t0) / 1e6f;
            ESP_LOGI(TAG, "fps %.1f (%d new) | sim %.0f steps/s, %" PRId64 " us/step (max %" PRId64
                     "), %d awake | snapshot %" PRId64 " us | render %" PRId64 " us | dma wait %" PRId64
                     " us | motion %.0f | bat %.0f mV %d %%%s%s%s",
                     (double)((float)frames / secs), new_frames, (double)((float)st.steps / secs),
                     st.steps ? st.step_us / st.steps : 0, st.max_step_us, st.awake,
                     st.snaps ? st.snap_us / st.snaps : 0, render_us / frames, wait_us / frames,
                     (double)st.motion, (double)bat.mv, bat.percent, bat.charging ? " (USB)" : bat.low ? " LOW" : "",
                     st.lost_us ? " | SIM BEHIND REAL TIME" : "", dim > 0.0f ? " | dimmed" : "");
#endif
            if (st.lost_us) {
                ESP_LOGW(TAG, "simulation lost %" PRId64 " ms of real time in the last second", st.lost_us / 1000);
            }
            console_tick();
            stat_t0 = t2;
            frames = new_frames = 0;
            render_us = wait_us = 0;
        }

        pace_frame(&next);
    }

    // Power off. Idle dimming may have the backlight low; the collapse plays
    // at the brightness it is.
    if (off_reason == POWER_OFF_CUTOFF) {
        play_cutoff_warning(fb, true);
    }
    play_power_off(fb);
    power_off(__atomic_load_n(&s_mode_req, __ATOMIC_ACQUIRE), s_imu_err == ESP_OK, off_reason);
}
#endif

// ---- MODE_LED_CAL --------------------------------------------------------------------
//
// The screen shows the palette's main colour; the LED shows a candidate for
// the palette's LED colour. Candidates step through hue and saturation around
// LED_COLOR_* (gamma-encoded HSV, the same space as the config value). Click:
// next candidate, double click: previous, long press: next palette. Every
// change prints the value to copy into config.h.

#if APP_MODE == MODE_LED_CAL
#define CAL_HUES  (2 * LEDCAL_HUE_STEPS + 1)
#define CAL_COUNT (CAL_HUES * (LEDCAL_SAT_STEPS + 1))

static uint32_t cal_candidate(const palette_t *pal, int idx, float *dh, float *sat)
{
    float h, s, v;
    ledcolor_to_hsv(pal->led, &h, &s, &v);
    *dh = (float)(idx % CAL_HUES - LEDCAL_HUE_STEPS) * LEDCAL_HUE_STEP_DEG;
    *sat = fmaxf(0.0f, s - (float)(idx / CAL_HUES) * LEDCAL_SAT_STEP);
    return ledcolor_from_hsv(h + *dh, *sat, v);
}

// Channel check: pure red, green and blue for a second each on the LED and
// the screen together. If the LED shows a different colour than the screen,
// BOARD_RGB_ORDER_RGB in board.h is wrong.
static void led_channel_check(uint16_t *fb)
{
    static const struct {
        const char *name;
        uint32_t rgb;
    } CH[] = { { "RED", 0xFF0000 }, { "GREEN", 0x00FF00 }, { "BLUE", 0x0000FF } };
    printf("ledcal: channel check, LED byte order %s: the LED must match the screen\n",
           BOARD_RGB_ORDER_RGB ? "RGB" : "GRB");
    for (size_t i = 0; i < sizeof(CH) / sizeof(CH[0]); i++) {
        printf("ledcal: channel check %s (screen and LED 0x%06" PRIX32 ")\n", CH[i].name, CH[i].rgb);
        led_show(CH[i].rgb, LEDCAL_LEVEL);
        display_wait();
        render_fill(fb, render_color(CH[i].rgb));
        render_overlay(fb, CH[i].name, 0xFFFFFF, 1.0f); // name in white on a black box
        display_push_async();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("ledcal: channel check done. If any colour differed, flip BOARD_RGB_ORDER_RGB in src/board.h.\n");
}

static void run_led_cal(void)
{
    uint16_t *fb = display_framebuffer();
    led_channel_check(fb);
    button_take_events(); // presses during the check do not count
    int pal_idx = 0;
    int idx = LEDCAL_HUE_STEPS; // the configured value: no hue offset, full saturation
    bool changed = true;
    int64_t label_t0 = esp_timer_get_time();

    printf("\nledcal: the screen shows the palette's main colour, the LED a candidate LED colour.\n");
    printf("ledcal: click = next candidate, double click = previous, long press = next palette.\n");
    printf("ledcal: %d candidates per palette: hue %+.0f..%+.0f deg in %.0f deg steps, saturation down to -%.1f.\n",
           CAL_COUNT, (double)(-LEDCAL_HUE_STEPS * LEDCAL_HUE_STEP_DEG), (double)(LEDCAL_HUE_STEPS * LEDCAL_HUE_STEP_DEG),
           (double)LEDCAL_HUE_STEP_DEG, (double)(LEDCAL_SAT_STEPS * LEDCAL_SAT_STEP));
    for (;;) {
        const uint32_t ev = button_take_events();
        if (ev & GESTURE_CLICK) {
            idx = (idx + 1) % CAL_COUNT;
            changed = true;
        }
        if (ev & GESTURE_DOUBLE) {
            idx = (idx + CAL_COUNT - 1) % CAL_COUNT;
            changed = true;
        }
        if (ev & GESTURE_LONG) {
            pal_idx = (pal_idx + 1) % PALETTE_COUNT;
            idx = LEDCAL_HUE_STEPS;
            changed = true;
            label_t0 = esp_timer_get_time();
        }
        const palette_t *pal = palette_get(pal_idx);
        float dh, sat;
        const uint32_t cand = cal_candidate(pal, idx, &dh, &sat);
        if (changed) {
            changed = false;
            led_show(cand, LEDCAL_LEVEL);
            const uint32_t sent = led_linear(cand, LEDCAL_LEVEL);
            printf("ledcal %s: candidate %d/%d, hue %+.0f deg, saturation %.2f -> "
                   "#define LED_COLOR_%s 0x%06" PRIX32 "   (configured 0x%06" PRIX32 "; LED gets R%" PRIu32
                   " G%" PRIu32 " B%" PRIu32 " at level %d)\n",
                   pal->name, idx + 1, CAL_COUNT, (double)dh, (double)sat, pal_idx == PALETTE_GREEN ? "GREEN" : "ORANGE",
                   cand, pal->led, (sent >> 16) & 0xFF, (sent >> 8) & 0xFF, sent & 0xFF, LEDCAL_LEVEL);
        }
        display_wait();
        render_fill(fb, render_color(pal->shade[0]));
        const int64_t shown_ms = (esp_timer_get_time() - label_t0) / 1000;
        if (shown_ms < OVERLAY_HOLD_MS + OVERLAY_FADE_MS) {
            const float alpha = shown_ms < OVERLAY_HOLD_MS
                                    ? 1.0f
                                    : 1.0f - (float)(shown_ms - OVERLAY_HOLD_MS) / (float)OVERLAY_FADE_MS;
            render_overlay(fb, pal->name, pal->shade[0], alpha);
        }
        display_push_async();
        console_tick();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#endif

// ---- Entry -----------------------------------------------------------------------------

static void app_task(void *arg)
{
    (void)arg;
#if APP_MODE == MODE_TEST_PATTERN
    run_test_pattern();
#elif APP_MODE == MODE_TILT_DOT
    run_tilt_dot();
#elif APP_MODE == MODE_SAND
    run_sand();
#elif APP_MODE == MODE_LED_CAL
    run_led_cal();
#else
#error "APP_MODE must be MODE_TEST_PATTERN, MODE_TILT_DOT, MODE_SAND or MODE_LED_CAL"
#endif
    vTaskDelete(NULL);
}

void app_main(void)
{
    power_init();
    if (power_is_arm_wake()) {
        // The brief timer wake after power-off: arm motion wake, sleep again.
        power_arm_wake();
    }
    ESP_LOGI(TAG, "starting, mode %s", APP_MODE_NAME);
    // LED first: a WS2812 keeps whatever it last latched (from a previous
    // firmware or noise on a floating line) until it is told otherwise.
    s_led_err = led_init();
    if (s_led_err != ESP_OK) {
        ESP_LOGE(TAG, "RGB LED init failed");
    }
    // Battery before the backlight: its first, settled reading (boot cutoff
    // check, drain log) is taken with the cell as lightly loaded as at
    // power-off.
    s_bat_err = battery_init();
    if (s_bat_err != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC init failed");
    }

    ESP_ERROR_CHECK(display_init());
    display_set_backlight(BACKLIGHT_PERCENT);

    // Everything is brought up in every mode so the banner can report it.
    s_imu_err = imu_init();
    if (s_imu_err != ESP_OK) {
        ESP_LOGE(TAG, "IMU init failed; sand will fall straight down");
    }
    s_btn_err = button_init();
    if (s_btn_err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed");
    }

#if APP_MODE == MODE_SAND
    ESP_ERROR_CHECK(sand_init());
#endif

    print_banner();

    xTaskCreatePinnedToCore(app_task, "app", APP_TASK_STACK, NULL, APP_TASK_PRIO, NULL, APP_TASK_CORE);
}
