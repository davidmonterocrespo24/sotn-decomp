// SOTN ESP32-S3 — the soft rasterizer's embedded shell.
//
// Compiles the same portable core the PC harness uses (soft_raster.inc.c,
// standalone flavor: it brings its own packet parser and Draw_* state) and
// binds VRAM to the game's g_RawVram, which lives in PSRAM by explicit
// attribute. Present/scanout is F3's job; F2 is headless.

#include <psyz.h>
#include <libgpu.h>
#include <string.h>
#include <stdlib.h>
#include "esp_attr.h"
#include "internal.h"
#include "draw.h"

// the game's VRAM: sotn.c defines it (SOTN_XRAM puts it in PSRAM)
extern u16 g_RawVram[VRAM_H * VRAM_W];

#include <stdio.h>
#define WARNF(fmt, ...) printf("[soft] " fmt "\n", ##__VA_ARGS__)
#define SOFT_RASTER_STANDALONE
#define SOFT_RASTER_NO_FALLBACK
#define SOFT_RASTER_IRAM IRAM_ATTR
#define SOFT_CLUT_CACHE
#define Draw_PushPrim Draw_PushPrim_impl
#include "platform/soft_raster.inc.c"
#undef Draw_PushPrim

// frame budget accounting: parse+raster cycles, read+reset by esp_shim
unsigned sotn_prim_cycles;
static inline unsigned rdccount(void) {
    unsigned c;
    __asm__ volatile("rsr.ccount %0" : "=a"(c));
    return c;
}
unsigned sotn_prim_census[8];     // by opcode top 3 bits: 1=poly 2=line 3=rect
unsigned sotn_prim_cycles_by[8];  // cycle split by the same classes
unsigned sotn_poly_kind[4];       // poly counts: [semi]*2 + [textured]
unsigned sotn_move_count, sotn_exeque_count, sotn_push_count, sotn_pkt_drops;
int Draw_PushPrim(u_long* packets, int max_len) {
    sotn_push_count++;
    unsigned t0 = rdccount();
    int r = Draw_PushPrim_impl(packets, max_len);
    unsigned dt = rdccount() - t0;
    unsigned cls = (packets[0] >> 29) & 7;
    sotn_prim_cycles += dt;
    sotn_prim_cycles_by[cls] += dt;
    sotn_prim_census[cls]++;
    if (cls == 1) {
        unsigned code = packets[0] >> 24;
        sotn_poly_kind[((code & 2) ? 2 : 0) | ((code & 4) ? 1 : 0)]++;
    }
    return r;
}

// the PC harness binds VRAM in InitPlatform (soft_gpu.c); here a constructor
// does it so the first LoadImage cannot hit a NULL vram
__attribute__((constructor)) static void BindVram(void) { Draw_Reset(); }

// F3 scanout: the freshly finished frame (see Sotn_DisplayOrigin)
const u16* Soft_DisplayPtr(void) {
    void Sotn_DisplayOrigin(int* x, int* y);
    int x = display_area_px.x, y = display_area_px.y;
    Sotn_DisplayOrigin(&x, &y);
    if (x < 0 || x > VRAM_W - 256 || y < 0 || y > VRAM_H - 240) {
        x = display_area_px.x;
        y = display_area_px.y;
    }
    return &g_RawVram[y * VRAM_W + x];
}

// visible rows per the game's draw clip: rows outside were PSX overscan
// (invisible on a TV) and hold stale bytes that flicker between buffers
void Soft_VisibleRows(int* y0, int* y1) {
    *y0 = st.y0;
    *y1 = st.y1;
}

void Draw_Reset() {
    vram = g_RawVram;
    st.x0 = 0;
    st.y0 = 0;
    st.x1 = VRAM_W - 1;
    st.y1 = VRAM_H - 1;
    st.ox = st.oy = 0;
}
