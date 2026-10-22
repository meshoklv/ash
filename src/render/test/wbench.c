/*
 * Zone draw A/B bench (the perf-night design notes): one zone package, a list of fixed cameras, drawn either the way the game
 * does today (src/world world_draw + world_upload_vram) or with src/render render_world (strips, cached lists,
 * sort, mips, hot VRAM). Built as ash_wbench_est (PSP-est, a PSPEST line per frame) by src/render/CMakeLists.txt.
 *
 * wb.txt next to the EBOOT:
 *   zone <file.pak>             mips <file.ashmip>
 *   mode A                      (world_draw, all-or-nothing world_upload_vram)  |  mode B <RW_* flags, hex> <vram 0/1>
 *   view <far> <lod> <fog_near> <fog_far> <fog_colour ABGR hex>   (optional, World defaults otherwise)
 *   frames <n>                  frames per camera (all analysed by PSP-est; the last one is dumped)
 *   cam <label> ex ey ez tx ty tz fov <collision name | * | auto>   (auto = floor part under the target)
 * Output: stdout "wb: f=<swap> cam=<label> mode=..." before each swap, wb_<label>.raw (RGB565 512x272) per camera.
 */
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcol.h"
#include "render.h"
#include "render_world.h"
#include "world.h"

PSP_MODULE_INFO("Ash wbench", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272

static unsigned int __attribute__((aligned(64))) g_list[512 * 1024];

typedef struct { char label[32], col[24]; float e[3], t[3], fov; } Cam;
static Cam g_cam[64];
static int g_ncam;

static void dump(void *draw, const char *label)
{
    char name[64];
    snprintf(name, sizeof(name), "wb_%s.raw", label);
    SceUID fd = sceIoOpen(name, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, (void *)(0x44000000 | (u32)draw), BUF_WIDTH * SCR_HEIGHT * 2);
    sceIoClose(fd);
}

int main(void)
{
    scePowerSetClockFrequency(333, 333, 166);
    char zone[64] = "zone.pak", mipf[64] = "", mode = 'A';
    unsigned rwf = RW_ALL;
    int vram = 1, frames = 3, has_view = 0;
    float vfar = 0, vlod = 0, fn = 0, ff = 0;
    unsigned vcol = 0;
    u8 *txt;
    int n = world_read_file("wb.txt", &txt);
    if (n <= 0) { printf("wb: no wb.txt\n"); sceKernelExitGame(); return 0; }
    txt[n] = 0;
    for (char *s = (char *)txt; s && *s;) {
        char *nl = strchr(s, '\n');
        if (nl) *nl = 0;
        Cam *c = &g_cam[g_ncam];
        if (!strncmp(s, "zone ", 5)) sscanf(s + 5, "%63s", zone);
        else if (!strncmp(s, "mips ", 5)) sscanf(s + 5, "%63s", mipf);
        else if (!strncmp(s, "mode ", 5)) sscanf(s + 5, "%c %x %d", &mode, &rwf, &vram);
        else if (!strncmp(s, "frames ", 7)) sscanf(s + 7, "%d", &frames);
        else if (!strncmp(s, "view ", 5)) has_view = sscanf(s + 5, "%f %f %f %f %x", &vfar, &vlod, &fn, &ff, &vcol) == 5;
        else if (!strncmp(s, "cam ", 4) && g_ncam < 64 &&
                 sscanf(s + 4, "%31s %f %f %f %f %f %f %f %23s", c->label, &c->e[0], &c->e[1], &c->e[2], &c->t[0], &c->t[1],
                        &c->t[2], &c->fov, c->col) == 9)
            g_ncam++;
        s = nl ? nl + 1 : NULL;
    }
    u8 *pak;
    int ps = world_read_file(zone, &pak);
    static World W;
    if (ps <= 0 || world_load(&W, pak, ps) != 0) { printf("wb: zone %s failed\n", zone); sceKernelExitGame(); return 0; }
    if (has_view) { W.far_dist = vfar; W.lod_dist = vlod; W.fog_near = fn; W.fog_far = ff; W.fog_color = vcol; }
    static Pcol C;
    int has_col = W.pcol && pcol_load(&C, W.pcol, W.pcol_size) == 0;
    static RenderMips M;
    int has_mips = 0;
    if (mipf[0]) { u8 *mb; int ms = world_read_file(mipf, &mb); has_mips = ms > 0 && render_mips_load(&M, mb, ms) == 0; }

    void *fbp0 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *fbp1 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *zbp = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_4444);
    int used = 3 * BUF_WIDTH * SCR_HEIGHT * 2, free_vram = sceGeEdramGetSize() - used;
    u8 *vbase = (u8 *)sceGeEdramGetAddr() + used;

    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_5650, fbp0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, fbp1, BUF_WIDTH);
    sceGuDepthBuffer(zbp, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CCW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CULL_FACE);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuEnable(GU_DITHER);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    render_init();

    static RenderWorld RW;
    if (mode == 'B') {
        int r = render_world_init(&RW, &W, has_mips ? &M : NULL, (int)rwf);
        printf("wb: render_world_init %d flags=%x: tris %d, list idx %d -> strip idx %d (%d of %d batches), lists %d KB, "
               "strips %d KB, mips %s\n", r, rwf, RW.tris, RW.list_idx, RW.strip_idx, RW.n_strip_batches, (int)W.w->n_batch,
               RW.dl_bytes / 1024, RW.strip_bytes / 1024, has_mips ? "yes" : "no");
        if (vram) render_world_vram(&RW, vbase, free_vram);
    } else if (vram) {
        int got = world_upload_vram(&W, vbase, free_vram);
        printf("wb: world_upload_vram %d KB\n", got / 1024);
    }
    printf("wb: mode=%c cams=%d frames=%d\n", mode, g_ncam, frames);

    void *draw = fbp0;
    int swap = 0;
    for (int ci = 0; ci < g_ncam; ci++) {
        Cam *c = &g_cam[ci];
        if (c->col[0] == '*') world_set_cell(&W, -1);
        else if (!strcmp(c->col, "auto")) {
            float h;
            int t = has_col ? pcol_floor(&C, c->t[0], c->t[2], c->t[1] + 1.0f, c->t[1] - 8.0f, 0.5f, &h) : -1;
            if (t < 0 && has_col) t = pcol_floor(&C, c->e[0], c->e[2], c->e[1] + 1.0f, c->e[1] - 8.0f, 0.5f, &h);
            if (t >= 0) world_set_cell_by_part(&W, C.tri[t].part); else world_set_cell(&W, -1);
        } else world_set_cell(&W, world_find_col(&W, c->col));
        for (int f = 0; f < frames; f++) {
            sceGuStart(GU_DIRECT, g_list);
            ScePspFMatrix4 proj, view;
            sceGumMatrixMode(GU_PROJECTION);
            sceGumLoadIdentity();
            sceGumPerspective(c->fov, 480.0f / 272.0f, 0.3f, 400.0f);
            sceGumStoreMatrix(&proj);
            sceGumMatrixMode(GU_VIEW);
            sceGumLoadIdentity();
            ScePspFVector3 e = {c->e[0], c->e[1], c->e[2]}, t = {c->t[0], c->t[1], c->t[2]}, up = {0, 1, 0};
            sceGumLookAt(&e, &t, &up);
            sceGumStoreMatrix(&view);
            sceGumMatrixMode(GU_MODEL);
            sceGumLoadIdentity();
            sceGumUpdateMatrix();
            sceGuClearColor(W.fog_color);
            sceGuClearDepth(0);
            sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
            WorldStats st;
            if (mode == 'B') render_world_draw(&RW, &proj, &view, c->e, &st);
            else world_draw(&W, &proj, &view, c->e, &st);
            sceGuFinish();
            sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
            printf("wb: f=%d cam=%s mode=%c tris=%d batches=%d tex=%d\n", swap, c->label, mode, st.tris, st.batches,
                   st.tex_changes);
            if (f == frames - 1) dump(draw, c->label);
            sceDisplayWaitVblankStart();
            draw = sceGuSwapBuffers();
            swap++;
        }
    }
    printf("wb: done\n");
    sceKernelExitGame();
    return 0;
}
