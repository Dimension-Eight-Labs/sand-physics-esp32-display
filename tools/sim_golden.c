// Golden test: material CLASSIC must reproduce "Classic physics v1" bit for bit.
//
//   cc -std=c11 -O2 -Isrc src/sim.c src/material.c tools/sim_golden.c -lm -o sim_golden && ./sim_golden
//
// Runs fixed-seed scenarios through the public API and hashes the complete
// state (cell grid, every grain's position/velocity bits, RNG state) at
// checkpoints. The expected hashes were captured from the v1 code before the
// material system existed (host x86-64, gcc -O2). Run with "print" to dump
// the hashes of the current build instead of comparing.
//
// Only gravity changes large enough to wake every grain under both the old
// and new wake rules are used, so the comparison isolates the physics.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "sim.h"

#define DEG (3.14159265f / 180.0f)

static sim_t sim;

static uint64_t fnv(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

static uint64_t state_hash(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv(h, sim.cell, (size_t)sim.w * sim.h * sizeof(uint16_t));
    h = fnv(h, sim.grains, (size_t)sim.n * sizeof(sim_grain_t));
    h = fnv(h, &sim.rng, sizeof(sim.rng));
    return h;
}

static void board(float lean_deg, float rot_deg)
{
    float in_plane = cosf(lean_deg * DEG);
    sim_set_gravity(&sim, in_plane * sinf(rot_deg * DEG), in_plane * cosf(rot_deg * DEG), -sinf(lean_deg * DEG));
}

static void run(int steps)
{
    for (int i = 0; i < steps; i++) {
        sim_step(&sim);
    }
}

typedef struct {
    const char *name;
    uint64_t expect;
} checkpoint_t;

// Captured from Classic physics v1.
static const checkpoint_t GOLDEN[] = {
    { "settle upright", 0xe5f079daa5191999ull },
    { "rotate 50", 0xe4a6cb6fd1e5b01eull },
    { "lean 60 rotate 70", 0x57974c4540e4d0deull },
    { "lean 75 rotate -90", 0x27b12e485e60001dull },
    { "shake + resettle", 0x6943c8a2f642223dull },
    { "tip past flat", 0xd3f1a6c72a007abaull },
    { "flat + noise", 0x7f71a97f13057c12ull },
    { "respawn rotate 180", 0xa222c65dec92036dull },
};
#define N_CHECK (sizeof(GOLDEN) / sizeof(GOLDEN[0]))

int main(int argc, char **argv)
{
    int print = argc > 1 && strcmp(argv[1], "print") == 0;
    uint64_t got[N_CHECK];
    int k = 0;

    int n = (int)(FILL_FRACTION * GRID_W * GRID_H);
    void *mem = malloc(sim_mem_bytes(GRID_W, GRID_H, n));
    sim_init(&sim, mem, GRID_W, GRID_H, n, 0xC1A55100u);
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);

    board(0, 0);
    run(1500);
    got[k++] = state_hash();

    board(0, 50);
    run(1500);
    got[k++] = state_hash();

    board(60, 70);
    run(1500);
    got[k++] = state_hash();

    board(75, -90);
    run(1500);
    got[k++] = state_hash();

    board(0, 0);
    for (int i = 0; i < 200; i++) {
        if ((i & 1) == 0) {
            sim_shake(&sim, 0.3f, -1.0f, 1.2f);
        }
        sim_step(&sim);
    }
    run(800);
    got[k++] = state_hash();

    board(120, 0);
    run(1200);
    got[k++] = state_hash();

    srand(7);
    for (int i = 0; i < 600; i++) {
        float nx = ((rand() % 2001) - 1000) / 1000.0f * 0.03f;
        float ny = ((rand() % 2001) - 1000) / 1000.0f * 0.03f;
        sim_set_gravity(&sim, nx, ny, -1.0f);
        sim_step(&sim);
    }
    got[k++] = state_hash();

    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    board(0, 180);
    run(1500);
    got[k++] = state_hash();

    int fails = 0;
    for (unsigned i = 0; i < N_CHECK; i++) {
        if (print) {
            printf("    { \"%s\", 0x%016llxull },\n", GOLDEN[i].name, (unsigned long long)got[i]);
        } else {
            int ok = got[i] == GOLDEN[i].expect;
            fails += !ok;
            printf("%-22s %016llx  %s\n", GOLDEN[i].name, (unsigned long long)got[i], ok ? "match" : "MISMATCH");
        }
    }
    if (!print) {
        printf(fails ? "CLASSIC differs from v1 in %d of %u checkpoints\n" : "CLASSIC matches Classic physics v1 bit for bit (%d/%u mismatches)\n",
               fails, (unsigned)N_CHECK);
    }
    free(mem);
    return fails ? 1 : 0;
}
