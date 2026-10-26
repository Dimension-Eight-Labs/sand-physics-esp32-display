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

#include "button.h"
#include "config.h"
#include "display.h"
#include "imu.h"
#include "led.h"
#include "palette.h"
#include "render.h"
#include "sim.h"

static const char *TAG = "app";

#define APP_TASK_STACK 6144
#define APP_TASK_PRIO  4
#define APP_TASK_CORE  1

#define FRAME_US   (1000000 / TARGET_FPS)
#define SIM_DT_US  (1000000 / SIM_HZ)

#if APP_MODE == MODE_TEST_PATTERN
#define APP_MODE_NAME "testpattern"
#elif APP_MODE == MODE_TILT_DOT
#define APP_MODE_NAME "tiltdot"
#elif APP_MODE == MODE_SAND
#define APP_MODE_NAME "sand"
#endif

#if APP_MODE == MODE_SAND
#define SIM_TASK_STACK 4096
#define SIM_TASK_PRIO  4   // below the IMU task (5) on the same core
#define SIM_TASK_CORE  0
static char s_mem_info[160]; // where the sand memory went, for the banner
#endif

// Init results, reported in the banner.
static esp_err_t s_imu_err = ESP_ERR_NOT_FINISHED;
static esp_err_t s_led_err = ESP_ERR_NOT_FINISHED;
static esp_err_t s_btn_err = ESP_ERR_NOT_FINISHED;

static void print_banner(void)
{
    imu_info_t imu = imu_get_info();
    printf("\n");
    printf("==================== phosphor sand ====================\n");
    printf(" mode      : %s (APP_MODE=%d)\n", APP_MODE_NAME, APP_MODE);
    printf(" firmware  : %s, built %s %s\n", esp_app_get_description()->version, __DATE__, __TIME__);
    printf(" ESP-IDF   : %s\n", esp_get_idf_version());
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
#if APP_MODE == MODE_SAND
    printf(" sand      : %s\n", s_mem_info);
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

#if APP_MODE != MODE_TEST_PATTERN
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
        led_update(0xFF0000, 0.5f, (uint32_t)(esp_timer_get_time() / 1000));
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
        float glass = BACK_FRICTION * fabsf(imu.gz);
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
        led_update(pal->led, sp / 8.0f, now_ms());

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

    sim_init(&s_sim, mem, GRID_W, GRID_H, n, esp_random());
    sim_spawn(&s_sim, SPAWN_REGION_FRACTION, PALETTE_SHADES);
    render_init(PALETTE_DEFAULT);
    return ESP_OK;
}

static void sim_task(void *arg)
{
    (void)arg;
    int64_t last = esp_timer_get_time();
    int64_t acc_us = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();
        acc_us += now - last;
        last = now;

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
            if (__atomic_exchange_n(&s_respawn_req, false, __ATOMIC_ACQ_REL)) {
                sim_spawn(&s_sim, SPAWN_REGION_FRACTION, PALETTE_SHADES);
            }

            uint32_t steps = 0;
            int64_t step_us = 0, max_us = 0;
            while (acc_us >= SIM_DT_US) {
                int64_t t0 = esp_timer_get_time();
                sim_step(&s_sim);
                int64_t dt = esp_timer_get_time() - t0;
                step_us += dt;
                max_us = dt > max_us ? dt : max_us;
                acc_us -= SIM_DT_US;
                steps++;
            }

            // Publish a snapshot if the renderer has taken the previous one.
            bool want;
            portENTER_CRITICAL(&s_snap_lock);
            want = !s_snap.fresh;
            portEXIT_CRITICAL(&s_snap_lock);
            int64_t snap_us = 0;
            if (want) {
                int64_t t0 = esp_timer_get_time();
                render_snapshot(&s_sim, s_snap.buf[s_snap.back]);
                snap_us = esp_timer_get_time() - t0;
                portENTER_CRITICAL(&s_snap_lock);
                int t = s_snap.back;
                s_snap.back = s_snap.ready;
                s_snap.ready = t;
                s_snap.fresh = true;
                portEXIT_CRITICAL(&s_snap_lock);
            }

            portENTER_CRITICAL(&s_stats_lock);
            s_stats.steps += steps;
            s_stats.step_us += step_us;
            s_stats.max_step_us = max_us > s_stats.max_step_us ? max_us : s_stats.max_step_us;
            s_stats.snap_us += snap_us;
            s_stats.snaps += want;
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

static void run_sand(void)
{
    uint16_t *fb = display_framebuffer();
    int64_t next = esp_timer_get_time();
    int64_t stat_t0 = next;
    int frames = 0, new_frames = 0;
    int64_t render_us = 0, wait_us = 0;

    xTaskCreatePinnedToCore(sim_task, "sim", SIM_TASK_STACK, NULL, SIM_TASK_PRIO, NULL, SIM_TASK_CORE);

    for (;;) {
        uint32_t ev = button_take_events();
        if (ev & BUTTON_EVT_SHORT) {
            __atomic_store_n(&s_respawn_req, true, __ATOMIC_RELEASE);
            render_clear();
            ESP_LOGI(TAG, "respawn");
        }
        if (ev & BUTTON_EVT_LONG) {
            render_set_palette(render_palette() + 1);
            ESP_LOGI(TAG, "palette: %s", palette_get(render_palette())->name);
        }

        // Take the newest snapshot, if there is one.
        portENTER_CRITICAL(&s_snap_lock);
        bool got = s_snap.fresh;
        if (got) {
            int t = s_snap.front;
            s_snap.front = s_snap.ready;
            s_snap.ready = t;
            s_snap.fresh = false;
        }
        portEXIT_CRITICAL(&s_snap_lock);
        new_frames += got;

        int64_t t0 = esp_timer_get_time();
        display_wait();
        int64_t t1 = esp_timer_get_time();
        render_frame(s_snap.buf[s_snap.front], fb);
        display_push_async();
        int64_t t2 = esp_timer_get_time();

        portENTER_CRITICAL(&s_stats_lock);
        float motion = s_stats.motion;
        portEXIT_CRITICAL(&s_stats_lock);
        led_update(palette_get(render_palette())->led, motion / LED_MOTION_FULL, now_ms());

        frames++;
        wait_us += t1 - t0;
        render_us += t2 - t1;
        if (t2 - stat_t0 >= 1000000) {
            sim_stats_t st;
            portENTER_CRITICAL(&s_stats_lock);
            st = s_stats;
            s_stats = (sim_stats_t){ .awake = st.awake, .motion = st.motion };
            portEXIT_CRITICAL(&s_stats_lock);
            float secs = (float)(t2 - stat_t0) / 1e6f;
            ESP_LOGI(TAG, "fps %.1f (%d new) | sim %.0f steps/s, %" PRId64 " us/step (max %" PRId64
                     "), %d awake | snapshot %" PRId64 " us | render %" PRId64 " us | dma wait %" PRId64
                     " us | motion %.0f%s",
                     (double)((float)frames / secs), new_frames, (double)((float)st.steps / secs),
                     st.steps ? st.step_us / st.steps : 0, st.max_step_us, st.awake,
                     st.snaps ? st.snap_us / st.snaps : 0, render_us / frames, wait_us / frames,
                     (double)st.motion, st.lost_us ? " | SIM BEHIND REAL TIME" : "");
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
#else
#error "APP_MODE must be MODE_TEST_PATTERN, MODE_TILT_DOT or MODE_SAND"
#endif
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting, mode %s", APP_MODE_NAME);
    // LED first: a WS2812 keeps whatever it last latched (from a previous
    // firmware or noise on a floating line) until it is told otherwise.
    s_led_err = led_init();
    if (s_led_err != ESP_OK) {
        ESP_LOGE(TAG, "RGB LED init failed");
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
