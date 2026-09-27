# SOTN → ESP32-S3: PHASE 1 PLAN

Target: dual Xtensa LX7 @240MHz, 512KB SRAM, 8MB octal PSRAM (~123-cycle miss), 64KB D-cache, SD card, 320x240 LCD. Prior art on this exact board: OpenLara TR1 48-63fps, Driver2 software rasterizer 16.8fps, GBA interpreter 42-47%. Source: sotn-decomp PC port (native recompile, no PSX address space — `tools/psyz/PORTING_GUIDE.md:96-98`; `u_long` stays 4B on 32-bit ESP32, matching PSX layout).

---

## 1. Memory budget — PSX-need vs S3-have

All sizes are 32-bit-build sizes (the sizes the S3 will actually see), from `nm`/declaration arithmetic in the surveys.

### PSRAM (8,388,608 B available)

| Object | Size (B) | Source |
|---|---:|---|
| g_RawVram (16-bit VRAM, 1024x512, render target + texture source) | 1,048,576 | `src/pc/sotn.c:17`, `internal.h:23-24` |
| g_TileDefDataPool **after mandatory u8 retype** | 1,048,576 | `stage_loader.c:64` vs `:46,53` |
| spu struct (incl. 512KB emulated SPU RAM, 24 voices) | 543,380 | `psyz_spu.c:111-128`, `spu.h:19` |
| VB sound bodies D_8013B6A0/D_8017D350/D_8018B4E0/D_801A9C80 | 499,776 | `src/pc/stubs.c:117-120` |
| Stage overlay .data+.bss, statically linked (worst case dai) | ~272,112 | objdump `dai.dll` |
| ARC spritesheet (persistent malloc, Alucard) | 203,756 | `pl_arc.c:14-20`, `sotn_err.log:44` |
| g_GpuBuffers[2] (2 × 0x177F4, PSX layout) | 192,488 | `game.h:962-979` |
| g_BmpCastleMap | 131,072 | `sotn.c:50` |
| g_LayoutPool | 131,072 | `stage_loader.c:66` |
| D_80280000 staging (VB/F_GO.BIN region) | 110,000 | `sim_pc.c:29-30` |
| g_PrimBuf[1280] (56 B/prim on 32-bit) | 71,680 | `primitive.h:49-90`, `game.h:275` |
| psyz queue_buf[0x4000] u_long | 65,536 | `libgpu.c:46` |
| D_psp_* ×4 | 65,536 | `stubs.c:101-104` |
| g_tracks | 52,668 | `libcd.c:50` |
| g_Entities[256] (0xBC each) | 48,128 | `game.h:937,2192` |
| aPbav VH buffers | 36,864 | `dra.h:566-569` |
| g_Pix / g_Clut / g_SpritePartPool / xa / g_PalEquipIcon / smaller statics | ~120,000 | `stubs.c`, `stage_loader.c:283`, `libcd.c` |
| Transient asset-load heap (FileAsString peak: largest file 270,336) | ~280,000 | `io.c:11,44` |
| **Total PSRAM** | **~4.6 MB** | |
| **Headroom** | **~3.7 MB** | |

### SRAM (512 KB available)

| Object | Size (B) | Why SRAM |
|---|---:|---|
| g_Scratchpad (SP_LEN 0x400) | 1,024 | It *is* the PSX 1KB scratchpad (`scratchpad.h:10`) — must be fast |
| Active CLUT cache (256×u16) + texpage state | ~1,024 | Per-texel CLUT lookup is the rasterizer inner loop |
| LCD scanout strips, 2 × 320×24×2, DMA-capable | 30,720 | esp_lcd DMA cannot source from PSRAM efficiently |
| cd_buf sector ring (18,816) — SD read target | 18,816 | `libcd.c:373-382`; SD DMA |
| Audio I2S DMA ring | ~16,384 | pull-model mixer output |
| Task stacks: game 32K, render 16K, audio 16K | 65,536 | note blocker R5 — no 512KB stack arrays |
| ESP-IDF/newlib/driver reserve | ~150,000 | measured on prior ports |
| **Total SRAM committed** | **~285 KB** | ~225 KB slack |

Flash (XIP): .text 4.23MB x64 will shrink on 32-bit Xtensa; .data/.rdata (~1.4MB) plus stage gen tables live in flash as const. Fits a 16MB-flash app partition; no code in SRAM except IRAM inner loops of the rasterizer.

Verdict: **fits, with ~45% PSRAM headroom** — but only after the tiledef retype (R3 below).

---

## 2. The REAL blockers, ranked by risk

**R1 — Rasterizer fill-rate over PSRAM VRAM (performance).** g_RawVram is 1MB against a 64KB D-cache with 123-cycle misses; every textured pixel does a texture fetch (indexed 4/8bpp) + CLUT lookup + framebuffer write, all potentially PSRAM. Per-frame worst case from GpuBuffer capacities: 0x300 GT4 quads + 0x280 SPRT_16 + rest (`game.h:170-178`) at 256×240. Mitigations that must work: 16-bit-native VRAM (halves traffic vs psyz's RGBA8888, `sdl3_gpu.c:1331,1370`), SRAM CLUT cache, tile-row-local access order, core-1 render task. Driver2 proved a full 3D rasterizer at 16.8fps on this board; SOTN is 2D at fixed 256×240, so the bet is sound — but this is the only blocker that can't be fixed by editing one file.

**R2 — Overlays are native DLLs.** `LoadStageOverlay` does LoadLibraryA/dlopen(RTLD_GLOBAL) with the exe built `--export-all-symbols` (`overlay.c:100-148`, `CMakeLists.txt:856-922`); stage code imports DRA globals as plain externs (`GAME_IMPORT` empty, `common.h:58-67`). No dynamic linker exists on the S3. Static linking is mandatory and collides: every stage defines identically-named internals (`EntityUpdates`, `entityLayoutHorizontal`, D_801xxxxx duplicated across stages since PSX overlays shared 0x80180000 — `stage_nz0.c:9-15`). Plan in §4. Build-system surgery, medium effort, fully deterministic.

**R3 — g_TileDefDataPool is 16MB on 32-bit as written.** `static TileDefinition g_TileDefDataPool[0x40][4][0x1000]` (`stage_loader.c:64`) where the code only ever reads 0x1000 *bytes* per plane (`:46,53`). TileDefinition = 4 pointers (`game.h:1335-1340`). Retype to `u8 [0x40][4][0x1000]` = 1MB. One-line fix, verified safe by both surveys, but without it the port literally does not fit 8MB. Do first, upstream-able.

**R4 — Audio pull is the game's timer clock and does SD I/O.** `Psyz_SpuPullSamples` calls `Psyz_RcntAdd`, which fires the libsnd 240Hz sequencer tick (RCnt2, 4233600/17640 — `psyz_spu.c:587`, `libetc.c:181-199`, `ssstart.c`) and freads CD sectors inside the pull (`libcd.c:399,543`). Even with audio output stubbed in F1-F3, RCnt must keep ticking or the sound engine's state machines desync. On S3: audio pump = core-1 task feeding I2S DMA, never an ISR. Known benign race (game writes SPU regs unlocked, `psyz_spu.c:244-280`) is replicated as-is.

**R5 — 512KB stack array.** `spu_reset_hot()` declares `u8 saved_ram[PSYZ_SPU_RAM_SIZE]` on the stack (`psyz_spu.c:148-153`). Instant fatal on any S3 task. Replace with a static PSRAM buffer or delete the hot-reset path. Trivial, but it will fire on first SPU init if forgotten.

**R6 — Two asset files are missing from the checkout.** `assets/dra/vb_0..3.bin` (499,776 B total) and `assets/dra/g_PalEquipIcon.bin` (10,240 B) fail to open in the run logs and the game limps on without common SFX (`sotn.c:218,265-278`). An extraction step must produce them before the SD image is complete. Not a proof-of-life blocker (game tolerates the failure) but a ship blocker.

**R7 — Music source is the raw 583MB bin/cue.** XA/CDDA stream from `disks/sotn.us.bin` via cue parse (`libcd.c:195-318`); the extracted `SD/XA_STR1`/`CD1.DA` are never opened. Deferred entirely: WRP needs `MU_NO_AUDIO` (`lba_stage.c:23,104`), and the sequencer (SsSeq) is already stubbed on PC (`libsnd.c:62-214`). Carry bin/cue on SD in F7 or repoint `open_track_at_cd_pos`.

Non-blockers, confirmed: no mmap/async I/O (all loads via 4 fopen helpers, `io.c:73-128` — remap to VFS/SD); saves are plain files in `bu00/`/`bu10/` dirs (`libcard.c:19-29`, `psyz.c:13-59` — maps to SD directories); threading is 1 audio thread + main (`sdl3_audio.c`); hermite4 float resampler is fine (S3 has FPU).

---

## 3. Render-layer replacement plan

**Seam: the 22-function `Draw_*` backend (`tools/psyz/psyz/src/draw.h:37-81`).** Keep decomp libgpu (`decomp/src/libgpu/*.c`) and psyz `libgpu.c` (Gpu table, GPU_cw, GPU_Enqueue, Psyz_GpuExeque) completely unchanged; replace only the SDL3 platform file (1535 lines, `sdl3_gpu.c`) plus VSync/present/input from `sdl3_common.h`. This is byte-for-byte the PsyCross→Driver2 pattern that already shipped on this board.

**VRAM model:** keep `g_RawVram` u16 1024×512 native RGB5551 in PSRAM as the one true VRAM. Delete both 5551↔8888 conversions (`sdl3_gpu.c:1331,1370`) and the 3×2MB GPU staging (`:26,297-312`). `MyClearImage`/`MyStoreImage` (`sotn.c:230-255`) already read/write it directly.

**GP0 opcodes required (from `Psyz_GpuExeque`, `libgpu.c:47-120`):** 0x02 fill-rect, 0x80 vram-move, 0xA0 image write, 0xC0 image read, 0xE1-E6 draw state, and prim opcodes 0x34/0x38/0x3C/0x50/0x60/0x64/0x7C + semi-trans variants.

**Primitive mix is tiny and 2D-only** (game funnels through fixed GpuBuffer arrays, `game.h:962-979`; zero POLY_F*/FT*, zero TILE_1/8/16, zero SPRT_8, zero GTE 3D — grep-verified 0 occurrences):

| Rasterizer routine | PSX prim | Game usage (occurrences) |
|---|---|---|
| Gouraud textured quad | POLY_GT4 (cap 0x300/frame) | 71 — entities |
| Gouraud textured tri | POLY_GT3 (0x30) | 3 |
| Gouraud quad | POLY_G4 (0x100) | 6 — warp gradient bg |
| Gouraud line | LINE_G2 (0x100) | 4 |
| 16×16 sprite | SPRT_16 (0x280) | 7 — the tilemap (`4CE2C.c:33`) |
| Free-size sprite | SPRT (0x200) | 10 |
| Flat rect | TILE (0x100) | 6 — fades |
| State packets | DR_MODE (0x400), DR_ENV (0x10) | 7 / 39 |

**Semantics that must be bit-faithful** (from `sdl3_common.h`/`psx.frag.glsl`): signed 11-bit coord wrap `s11()` (`:778`); FixupFlipUV for mirrored sprites (`:816-838`); flat-shading RGB propagation when not gouraud (`sdl3_gpu.c:1037-1040`); semi-trans flag as alpha 0x80 (`:993`); TPAGE_NOTEXTURE 0x8000; texpage latch from E1/DR_MODE (`:840-849`); CLUT at `((clut%64)*16, clut/64)` with 4/8bpp index extraction done per-texel at draw time (`psx.frag.glsl:49-58`) — no TIM parsing anywhere, banks stream via LoadPendingGfx (`4AEA4.c:247-252`); PS1 modulation `(tex5*col8)>>7` clamp 31; all 4 ABR modes (B/2+F/2, B+F, B−F, B+F/4). 4×4 dither: skip initially (destination is RGB565 LCD anyway).

**Function surface (call counts over src/dra+src/st):** Tier 1 full: LoadTPage(70), LoadImage(48), SetDrawEnv(35), ClearImage(31), StoreImage(31), AddPrim/addPrim(44), DrawSync(18), MoveImage(16), SetDrawMode(15), VSync(13), SetSemiTrans(32), GetClut(9), LoadClut(7), PutDispEnv(6), SetShadeTex(27), PutDrawEnv(4), SetDispMask(4). Tier 2 singletons: DrawOTag, ClearOTag, ResetGraph, VSyncCallback (no-op on PC already, `libetc.c:52-55`), GsGetVcount, GsClearVcount. Tier 3 stub: FntPrint(244)/FntFlush — keep as serial printf.

**Hooks:** rasterize at DrawSync (`psyz_sync`→`Psyz_GpuExeque`, `libgpu.c:261-269`); present at VSync (`libapi.c:51-69`): copy `display_area` (256×240 stage / 384×240 menu / 512×240 title, `game.h:180-186`) from g_RawVram, convert 5551→565, DMA to LCD. Phase 1: 256×240 pillarboxed on the 320×240 panel; 384/512 modes deferred (direct `--stage` boot skips title/menu). Frame pacing: replace WaitForNextFrame's 59.94fps hybrid limiter (`sdl3_common.h:276-314`) with esp_timer target of 16,683µs; game logic is fixed-timestep 1-tick-per-VSync with real-vblank clock scaling already in place (`42398.c:984-1005`), so dropped frames degrade gracefully.

---

## 4. Overlay strategy — no-DLL target

**Static link, name→InitStage table.** Chosen over a bin-loader, which would need full two-way symbol+relocation infrastructure the PC port delegates to the host linker (imports are plain externs of *every* exe symbol — `common.h:58-67`, `CMakeLists.txt:859-880`). Rejected outright.

Mechanics, in order:
1. **Per-stage symbol isolation.** Each stage's public table is already namespaced (`OVL_EXPORT(x)` → `NZ0_##x`, `nz0.h:6`); internals are not (`EntityUpdates`, `entityLayoutHorizontal/Vertical`, shared D_801xxxxx — `stage_nz0.c:9-15`). Per stage: partial-link (`ld -r`) all its objects, then `objcopy --keep-global-symbol=<STG>_InitStage --localize-hidden` (or `--prefix-symbols` + a small allowlist). Result: one .o per stage exporting exactly one symbol, mirroring the DLL contract (1 export: `overlay.c:141`, `stage_nz0.c:34`).
2. **Rename entry points** `InitStage` → `wrp_InitStage`, `nz0_InitStage`, `tt_000_InitServant`, etc.
3. **Replace `LoadStageOverlay`** (`overlay.c:100-148`): swap dlopen/GetProcAddress for a const table `{ "wrp", wrp_InitStage } ...` keyed by `g_StagesLba[g_StageId].ovlName` (load path: `sim_pc.c:389-399,498-516`).
4. **Everything else is untouched.** InitStage memcpys the stage table into `g_api.o` and writes 3 DRA globals (PfnEntityUpdates, g_pStObjLayoutHorizontal/Vertical — `stage_nz0.c:37-39`); with static linking those externs resolve at link time, zero relocation.
5. **Footprint:** only one stage is *active* at a time but all linked stages' .data/.bss coexist. Per-stage .data ≈ 210-272KB (nz0/dai measurements). Phase 1 links wrp only (+tt_000 servant); each added stage costs ~0.2-0.3MB PSRAM .bss + flash .text (largest .text 155KB, dai). All 11 stages ≈ +2.5MB PSRAM — still inside headroom, but link them incrementally. Full set today: sel, no2, dai, cen, nz0, wrp, dre, st0, rwrp, no3, mad + tt_000-002 (`CMakeLists.txt:924-938`).
6. **cJSON stage_loader path is dead code** (zero callers repo-wide) — all room/layout/sprite data compiles in from `src/st/*/gen`; do not port cJSON.

---

## 5. Proof-of-life target: WRP (Warp Room), `--stage wrp`

Why WRP, not NZ0: 28 .c files / 55KB vs 277KB; direct boot bypassing title/menu (`main.c:14` index 14 = STAGE_WRP 0x0E; `sim_pc.c:499-515` sets g_StageId and jumps to Game_NowLoading); only 2 spawned entity kinds (E_RED_DOOR, E_WARP — `gen/e_layout.c:5-20`), zero enemies/bosses/cutscenes; `MU_NO_AUDIO` so no music path (`lba_stage.c:23,104`); smallest psyz surface — GT4/G4/TILE prims via AllocPrimitives, PlaySfx, no GTE, no MoveImage/StoreImage (`warp.c:74,108,117`; grep-verified). NZ0 is the second target: it adds MoveImage/StoreImage/RotMatrix/TransMatrix + cutscene engine (`cutscene.c`, `e_stage_name.c`) and real combat (70 entity handlers, Slogra+Gaibon).

**Exact SD manifest for WRP boot** (from verified run-log open sequence + path construction `sim_pc.c:411-423,529,552-563,592-653`, `weapon_pc.c:127-134`):

| File | Bytes | Purpose |
|---|---:|---|
| `disks/us/BIN/F_GAME.BIN` | 270,336 | shared game textures (fileId 1) |
| `disks/us/BIN/F_TITLE0.BIN` | 262,144 | opened during boot path |
| `disks/us/BIN/F_TITLE1.BIN` | 262,144 | opened during boot path |
| `disks/us/BIN/ARC_F.BIN` | 203,756 | Alucard spritesheet (persistent heap) |
| `disks/us/BIN/WEAPON0.BIN` | 1,691,648 | 16KB reads at fileId·0x7000 |
| `disks/us/VAB/SD_ALK.VH` | 4,128 | Alucard voice bank header |
| `disks/us/VAB/SD_ALK.VB` | 57,696 | Alucard voice bank body |
| `disks/us/ST/WRP/F_WRP.BIN` | 262,144 | stage tileset (SIM_STAGE_CHR) |
| `disks/us/ST/WRP/SD_ZKWRP.VH` | 7,200 | stage SFX header (0x1C20, `lba_stage.c:104`) |
| `disks/us/ST/WRP/SD_ZKWRP.VB` | 106,592 | stage SFX body (via D_80280000 staging) |
| `assets/dra/vb_0..3.bin` | 499,776 | **must extract** (R6); boot survives absence |
| `assets/dra/g_PalEquipIcon.bin` | 10,240 | **must extract** (R6); boot survives absence |
| **Total (excl. missing)** | **~3.13 MB** | |

Not needed: `WRP.BIN` (83,816 — PSX overlay, never opened on PC), NZ0 anything, DEMOKEY.BIN, SERVANT/*, sotn.us.bin. Stage code = statically linked `SOURCE_FILES_STAGE_WRP` (32 sources + glue, `CMakeLists.txt:499-531`, excluding st_init_psp.c).

**Visible success criterion:** the animated rainbow-gradient warp background (POLY_G4) renders on the LCD, Alucard stands in a 1-screen room, and the warp (E_WARP) teleports between all 5 warp rooms through the mirror/door rooms — 15 rooms in `gen/rooms.c`, 6 layouts in `gen/e_laydef.c`.

---

## 6. Phases F1..F8, falsifiable gates

**F1 — "S3-shape" build on PC (32-bit, no SDL, no DLLs).**
Retype g_TileDefDataPool to u8 (`stage_loader.c:64`); static-link wrp+tt_000 per §4; write the software rasterizer + Draw_* backend of §3 over 16-bit g_RawVram; present = dumb blit of g_RawVram display_area to a window; fix R5 stack array.
**Gate:** `sotn --stage wrp` renders the warp room via the new rasterizer; A/B screenshots vs the sdl3_gpu build match per-primitive (background gradient, tilemap, Alucard, fades, all 4 ABR modes exercised via the white-fade TILE with DRAW_TRANSP); process high-water RSS of game statics ≤ 5MB. Fail = pixels wrong or memory over.

**F2 — Cross-compile core, headless boot on S3.**
ESP-IDF project; PSRAM placement per §1 table (EXT_RAM_BSS attrs on the big arrays); io.c remapped to SD VFS; audio fully stubbed but RCnt ticked from a 240Hz esp_timer (R4); manifest of §5 on SD.
**Gate:** serial log shows InitStage("wrp") return, then ≥600 consecutive MainGame iterations (ClearOTag→UpdateGame→DrawOTag) with a printed per-frame heap/PSRAM report never exceeding the §1 budget and no watchdog. Fail = any panic, stack overflow, or budget breach in 10s of frames.

**F3 — First light + input.**
LCD scanout (5551→565, 256-wide pillarbox, DMA strips from SRAM); ReadPads (`pads.c:20-85`) backed by board buttons mapped to the PSX pad frame format (`libetc.c:29-46`).
**Gate:** photo of the LCD showing the animated warp gradient; Alucard walks/jumps under button control; warping visits all 5 rooms; 5 minutes without crash. Fail = any hang, torn scanout, or missed warp.

**F4 — Performance pass.**
IRAM the rasterizer inner loops; SRAM CLUT cache; profile PSRAM stall share; move rasterize+scanout to core 1 (game logic core 0), double-buffered via g_GpuBuffers' existing swap (`42398.c:871-947`).
**Gate:** on-screen fps counter shows ≥30fps sustained in WRP (target 60; 30 is the pass/fail line — Driver2 shipped at 16.8 in 3D, 2D must beat it 2x). Fail = <30fps after the core split.

**F5 — NZ0: real gameplay.**
Link nz0; implement MoveImage/StoreImage VRAM transfers and RotMatrix/TransMatrix (used by `cutscene.c`/`e_stage_name.c`); manifest += F_NZ0.BIN 262,144 + SD_ZKNZ0.VH 6,176 + VB 56,976.
**Gate:** boot `--stage nz0`, stage-name banner displays, kill one bloody zombie, reach and survive 30s of the Slogra+Gaibon fight, ≥25fps during the boss. Fail = missing gfx from unimplemented VRAM ops or fps floor broken.

**F6 — Audio: SPU on core 1.**
psyz software SPU (`psyz_spu.c`) pulled by I2S DMA task; Psyz_RcntAdd driven from the pull (replacing the F2 timer); VB/VH via existing synchronous SsVabTransBody path (`sim_pc.c:269-282`); extract R6 assets.
**Gate:** jump/attack/warp SFX audible in WRP and NZ0 with no underruns over 5 min (DMA underrun counter = 0) and frame rate unchanged from F4/F5 gates. Fail = underruns, or game-loop fps drop >10%.

**F7 — XA music streaming.**
CD image (or repointed extracted stream) on SD; libcd XA decode path (`libcd.c:431-776`) with SD reads inside the audio pull kept off-ISR; NZ0 requires MU_DANCE_OF_GOLD (`lba_stage.c:21`).
**Gate:** Dance of Gold plays looped in NZ0 while holding the F5 fps gate; SD contention does not underrun audio. Fail = stutter or fps regression.

**F8 — Saves.**
libcard's bu00/bu10 directory model (`psyz.c:13-59`) onto SD paths.
**Gate:** save in NZ0 save room, hard power-cycle, continue from the save with inventory intact. Fail = corrupt or missing save.

Phase 1 = F1-F4: **WRP on glass at ≥30fps with buttons** — the SOTN warp room running on a microcontroller.