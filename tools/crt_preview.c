// Host preview: the CRT power-off and power-on animations (no ESP-IDF needed).
//
//   mkdir -p build-host/crt && cc -std=c11 -O2 -Isrc src/sim.c src/material.c src/palette.c src/render.c
//       tools/crt_preview.c -lm -o build-host/crt_preview && build-host/crt_preview [palette]
//
// Writes build-host/crt/off_NNN.ppm and on_NNN.ppm (NNN = ms into the
// animation, every frame at TARGET_FPS), the hold-to-power-off bar at 50 %
// as bar.ppm, and the battery OSD with 0..5 bars (bat_N.ppm) and half faded
// (bat_fade.ppm). The power-off frames start from a settled pile; the power-on
// frames run the simulation from a fresh spawn, as on wake.
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "material.h"
#include "palette.h"
#include "render.h"
#include "sim.h"

static sim_t sim;
static uint16_t fb[SCREEN_W * SCREEN_H];
static uint8_t snap[GRID_W * GRID_H];

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint16_t c = fb[i];
#if BOARD_LCD_SWAP_BYTES
        c = (uint16_t)((c >> 8) | (c << 8));
#endif
        unsigned char rgb[3] = { (unsigned char)(((c >> 11) & 0x1F) << 3), (unsigned char)(((c >> 5) & 0x3F) << 2),
                                 (unsigned char)((c & 0x1F) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void steps(int n)
{
    for (int i = 0; i < n; i++) {
        sim_step(&sim);
    }
}

int main(int argc, char **argv)
{
    const int pal = argc > 1 ? atoi(argv[1]) : PALETTE_GREEN;
    const int n = (int)(FILL_FRACTION * GRID_W * GRID_H);
    void *mem = malloc(sim_mem_bytes(GRID_W, GRID_H, n));
    sim_init(&sim, mem, GRID_W, GRID_H, n, 7);
    sim_set_material(&sim, material_get(MATERIAL_SAND));
    sim_set_gravity(&sim, 0.3f, 0.95f, 0.0f);
    sim_spawn(&sim, SPAWN_REGION_FRACTION, PALETTE_SHADES);
    render_init(pal);
    render_set_look(pal, material_get(MATERIAL_SAND)->shade_spread);

    // Settled pile, a few frames of glow.
    steps(SIM_HZ * 6);
    for (int i = 0; i < 10; i++) {
        steps(SIM_HZ / TARGET_FPS);
        render_snapshot(&sim, snap);
        render_frame(snap, fb);
    }
    write_ppm("build-host/crt/before.ppm");
    render_progress(fb, 0.5f, palette_brightest(palette_get(pal)));
    render_overlay(fb, "OFF", palette_brightest(palette_get(pal)), 1.0f);
    write_ppm("build-host/crt/bar.ppm");
    // Battery OSD, 0..5 bars, and one frame mid-fade.
    for (int b = 0; b <= 5; b++) {
        char name[48];
        render_frame(snap, fb);
        render_battery(fb, b, palette_brightest(palette_get(pal)), 1.0f);
        snprintf(name, sizeof(name), "build-host/crt/bat_%d.ppm", b);
        write_ppm(name);
    }
    render_frame(snap, fb);
    render_battery(fb, 3, palette_brightest(palette_get(pal)), 0.5f);
    write_ppm("build-host/crt/bat_fade.ppm");

    float open, width, k;
    char path[64];
    for (uint32_t ms = 0;; ms += 1000 / TARGET_FPS) {
        bool more = render_crt_off_at(ms, &open, &width, &k);
        render_crt(fb, open, width, k);
        snprintf(path, sizeof(path), "build-host/crt/off_%03u.ppm", ms);
        write_ppm(path);
        if (!more) {
            break;
        }
    }

    // Wake: fresh spawn, the sim runs during the warm-up.
    sim_spawn(&sim, SPAWN_REGION_FRACTION, PALETTE_SHADES);
    sim_set_gravity(&sim, 0.0f, 1.0f, 0.0f);
    render_clear();
    for (uint32_t ms = 0;; ms += 1000 / TARGET_FPS) {
        steps(SIM_HZ / TARGET_FPS);
        render_snapshot(&sim, snap);
        bool more = render_crt_on_at(ms, &open, &width, &k);
        if (more) {
            render_merge(snap);
            render_crt(fb, open, width, k);
        } else {
            render_frame(snap, fb);
        }
        snprintf(path, sizeof(path), "build-host/crt/on_%03u.ppm", ms);
        write_ppm(path);
        if (!more) {
            break;
        }
    }
    free(mem);
    return 0;
}
