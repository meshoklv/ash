/*
 * Ash World test: renders zone_m18_01.pak (src/world) from a list of cameras and dumps every camera's frame as raw
 * RGB565 (fr_NNNN.raw, 512x272) next to the EBOOT. scripts/shoot-world.sh stages the files and converts the frames.
 *
 *   cams.txt   one camera per line:  <label> ex ey ez  tx ty tz  fov_deg  <collision name | *>
 *              (PSP space; "*" = draw every draw group). Written by tools/world_cams.py.
 *
 * Prints per camera: triangles / batches / texture changes drawn, the visibility cell, GE time (emulator!).
 * On the PSP: L/R = previous/next camera, the same file is read from ms0:/ash/.
 */
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world.h"
#include "../game/pcol.h"   /* the zone .pcol is pcol v1: check floor -> part -> visibility cell */

PSP_MODULE_INFO("AshWorld", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272
#define MAX_CAMS 64

static unsigned int __attribute__((aligned(64))) g_list[512 * 1024 / 4];

typedef struct { char label[32]; float eye[3], at[3], fov; char col[24]; } Cam;
static Cam g_cams[MAX_CAMS];

static int exit_cb(int a, int b, void *c) { (void)a; (void)b; (void)c; sceKernelExitGame(); return 0; }
static int cb_thread(SceSize a, void *b)
{
    (void)a; (void)b;
    int cbid = sceKernelCreateCallback("exit", exit_cb, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static int load_cams(void)
{
    u8 *buf;
    int n = world_read_file("cams.txt", &buf);
    if (n <= 0) return 0;
    buf[n] = 0;
    int k = 0;
    char *s = (char *)buf;
    while (*s && k < MAX_CAMS) {
        char *e = strchr(s, '\n');
        if (e) *e = 0;
        Cam *c = &g_cams[k];
        if (s[0] != '#' && sscanf(s, "%31s %f %f %f %f %f %f %f %23s", c->label, &c->eye[0], &c->eye[1], &c->eye[2],
                                  &c->at[0], &c->at[1], &c->at[2], &c->fov, c->col) == 9)
            k++;
        if (!e) break;
        s = e + 1;
    }
    free(buf);
    return k;
}

static void dump_frame(void *draw, int n)
{
    char name[32];
    snprintf(name, sizeof(name), "fr_%04d.raw", n);
    SceUID fd = sceIoOpen(name, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, (void *)(0x44000000 | (u32)draw), BUF_WIDTH * SCR_HEIGHT * 2);
    sceIoClose(fd);
}

int main(void)
{
    SceUID th = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
    printf("ash-world: free %d KB (max block %d KB)\n", sceKernelTotalFreeMemSize() / 1024,
           sceKernelMaxFreeMemSize() / 1024);

    u8 *pak;
    u64 t0, t1;
    sceRtcGetCurrentTick(&t0);
    int size = world_read_file("zone_m18_01.pak", &pak);
    World w;
    int rc = size > 0 ? world_load(&w, pak, size) : -100;
    sceRtcGetCurrentTick(&t1);
    if (rc) { printf("ash-world: load failed %d (size %d)\n", rc, size); sceKernelExitGame(); return 0; }
    printf("ash-world: pak %d KB loaded in %u ms: %u tex, %u batches, %u meshes, %u verts, %u tris; "
           "%u cols, %u objs, %u chrs, %u regions; pcol %d KB, %d parts\n",
           size / 1024, (unsigned)((t1 - t0) / 1000), (unsigned)w.w->n_tex, (unsigned)w.w->n_batch,
           (unsigned)w.w->n_mesh, (unsigned)w.w->n_vert, (unsigned)(w.w->n_idx / 3), (unsigned)w.z->n_col,
           (unsigned)w.z->n_obj, (unsigned)w.z->n_chr, (unsigned)w.z->n_reg, w.pcol_size / 1024, w.n_part);
    printf("ash-world: after load free %d KB (max block %d KB)\n", sceKernelTotalFreeMemSize() / 1024,
           sceKernelMaxFreeMemSize() / 1024);
    {
        u8 *v;   /* optional view.txt: "<far_dist> <lod_dist> <fog_near> <fog_far>" */
        int n = world_read_file("view.txt", &v);
        if (n > 0) {
            v[n] = 0;
            sscanf((char *)v, "%f %f %f %f", &w.far_dist, &w.lod_dist, &w.fog_near, &w.fog_far);
            free(v);
        }
        printf("ash-world: far %.0f m, lod %.0f m, fog %.0f..%.0f m\n", w.far_dist, w.lod_dist, w.fog_near, w.fog_far);
    }
    static Pcol col;
    int has_col = w.pcol && pcol_load(&col, w.pcol, w.pcol_size) == 0;
    printf("ash-world: pcol %s (%u tris, %u parts)\n", has_col ? "loaded" : "FAILED",
           has_col ? (unsigned)col.hdr->n_tri : 0u, has_col ? (unsigned)col.hdr->n_part : 0u);
    int ncam = load_cams();
    printf("ash-world: %d cameras\n", ncam);

    void *fbp0 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *fbp1 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *zbp = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_4444);
    {
        /* textures into the rest of VRAM */
        int used = 3 * BUF_WIDTH * SCR_HEIGHT * 2;
        int free_vram = sceGeEdramGetSize() - used;
        int got = world_upload_vram(&w, (u8 *)sceGeEdramGetAddr() + used, free_vram);
        printf("ash-world: textures %s VRAM (%d KB of %d KB free)\n", got ? "in" : "NOT in", got / 1024, free_vram / 1024);
    }
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
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    pspDebugScreenInitEx((void *)(0x44000000 | (u32)fbp0), PSP_DISPLAY_PIXEL_FORMAT_565, 0);
    pspDebugScreenEnableBackColor(0);
    sceCtrlSetSamplingCycle(0);

    int auto_mode = 0;
    {
        u8 *x;
        int n = world_read_file("auto.txt", &x);   /* present = headless run: dump every camera once, exit */
        if (n >= 0) { auto_mode = 1; free(x); }
    }
    int cam = 0, frame = 0, dumped = 0;
    void *draw = fbp0;
    u32 last_buttons = 0;
    while (ncam > 0) {
        Cam *c = &g_cams[cam];
        if (c->col[0] == '*') world_set_cell(&w, -1);
        else world_set_cell(&w, world_find_col(&w, c->col));
        char floor_eye[20] = "-", floor_at[20] = "-";
        if (has_col && auto_mode && frame == 1) {
            /* the game path: floor triangle under a point -> its part -> world_set_cell_by_part */
            float h;
            int t = pcol_floor(&col, c->eye[0], c->eye[2], c->eye[1] + 1.0f, c->eye[1] - 8.0f, 0.5f, &h);
            if (t >= 0) snprintf(floor_eye, sizeof(floor_eye), "%.15s", col.part[col.tri[t].part].name);
            t = pcol_floor(&col, c->at[0], c->at[2], c->at[1] + 1.0f, c->at[1] - 8.0f, 0.5f, &h);
            if (t >= 0) snprintf(floor_at, sizeof(floor_at), "%.15s", col.part[col.tri[t].part].name);
        }

        u64 a, b, g;
        sceRtcGetCurrentTick(&a);
        sceGuStart(GU_DIRECT, g_list);
        sceGuClearColor(w.fog_color);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        ScePspFMatrix4 proj, view;
        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadIdentity();
        sceGumPerspective(c->fov, 480.0f / 272.0f, 0.3f, 400.0f);
        sceGumStoreMatrix(&proj);
        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        {
            ScePspFVector3 eye = {c->eye[0], c->eye[1], c->eye[2]}, at = {c->at[0], c->at[1], c->at[2]},
                           up = {0, 1, 0};
            sceGumLookAt(&eye, &at, &up);
        }
        sceGumStoreMatrix(&view);
        WorldStats st;
        world_draw(&w, &proj, &view, c->eye, &st);
        sceGuFinish();
        sceRtcGetCurrentTick(&b);
        sceGuSync(0, 0);
        sceRtcGetCurrentTick(&g);
        pspDebugScreenSetOffset((int)draw);
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("%s  tris %d  batches %d  tex %d  cell %s", c->label, st.tris, st.batches,
                             st.tex_changes, w.cur_col >= 0 ? w.col[w.cur_col].name : "*");
        if (auto_mode && frame == 1) {
            dump_frame(draw, dumped++);
            printf("ash-world: cam %d %s tris=%d batches=%d tex=%d objs=%d tested=%d cell=%s pcol_floor eye=%s at=%s "
                   "cpu_list=%.2fms ge=%.2fms (emulator)\n",
                   cam, c->label, st.tris, st.batches, st.tex_changes, st.objs, st.tested,
                   w.cur_col >= 0 ? w.col[w.cur_col].name : "*", floor_eye, floor_at, (b - a) / 1000.0f,
                   (g - b) / 1000.0f);
        }
        sceDisplayWaitVblankStart();
        draw = sceGuSwapBuffers();
        frame++;
        if (auto_mode) {
            if (frame >= 2) {
                frame = 0;
                if (++cam >= ncam) break;
            }
        } else {
            SceCtrlData pad;
            sceCtrlPeekBufferPositive(&pad, 1);
            u32 nb = pad.Buttons & ~last_buttons;
            last_buttons = pad.Buttons;
            if (nb & PSP_CTRL_RTRIGGER) cam = (cam + 1) % ncam;
            if (nb & PSP_CTRL_LTRIGGER) cam = (cam + ncam - 1) % ncam;
        }
    }
    printf("ash-world: done, %d frames dumped\n", dumped);
    sceKernelExitGame();
    return 0;
}
