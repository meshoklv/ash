/*
 * Ash viewer v0: loads a cooked `.ashm` scene (tools/cook_scene.py) and renders it with sceGu.
 *   RGB565 framebuffers + dither, Z16, CLUT8 swizzled textures, indexed float vertices, back-face culling,
 *   per-batch sphere frustum culling, linear fog, FPS overlay.
 *
 * Files are looked up next to the EBOOT (cwd), then umd0:/, host0:/, ms0:/ash/:
 *   scene.ashm   - the cooked scene
 *   camera.txt   - optional, index of the camera stored in the scene (default 0)
 * Controls (hardware): L/R = previous/next camera, START = exit.
 * Stdout: "ash-viewer: ..." lines (PPSSPPHeadless log). Emulated FPS says nothing about real hardware.
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <psprtc.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>

PSP_MODULE_INFO("Ash Viewer", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272

static unsigned int __attribute__((aligned(16))) g_list[256 * 1024];

/* ---------------------------------------------------------------- .ashm v1 */
typedef struct {
    char magic[4];
    u32 version, n_tex, n_batch, n_vert, n_idx, n_cam, flags;
    u32 off_tex, off_batch, off_vert, off_idx, off_cam;
    float bbox_min[3], pad;
} AshmHeader;

typedef struct { u16 w, h, psm, flags; u32 off_clut, off_pix; } AshmTex;
typedef struct { u16 tex, flags; u32 vert_start, vert_count, idx_start, idx_count; float cx, cy, cz, radius; } AshmBatch;
typedef struct { float u, v; u32 color; float x, y, z; } AshmVert;
typedef struct { float eye[3], target[3], fov, pad; } AshmCam;

#define BF_ALPHA 1
#define BF_NOCULL 2

static u8 *g_file;
static AshmHeader *g_hdr;
static AshmTex *g_tex;
static AshmBatch *g_batch;
static AshmVert *g_vert;
static u16 *g_idx;
static AshmCam *g_cam;

static volatile int g_running = 1;
static int exit_cb(int a, int b, void *c) { (void)a; (void)b; (void)c; g_running = 0; return 0; }
static int cb_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exit_cb, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void)
{
    int th = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
}

static const char *g_prefixes[] = {"", "umd0:/", "host0:/", "ms0:/ash/", "disc0:/", NULL};

/* Reads a whole file into a 64-byte aligned buffer. Returns size or -1. */
static int load_file(const char *name, u8 **out, char *found, int found_len)
{
    char path[256];
    for (int i = 0; g_prefixes[i]; i++) {
        snprintf(path, sizeof(path), "%s%s", g_prefixes[i], name);
        SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
        if (fd < 0) continue;
        int size = (int)sceIoLseek(fd, 0, PSP_SEEK_END);
        sceIoLseek(fd, 0, PSP_SEEK_SET);
        u8 *buf = memalign(64, size + 16);
        if (!buf) { sceIoClose(fd); printf("ash-viewer: out of memory for %s (%d B)\n", path, size); return -1; }
        int got = 0;
        while (got < size) {
            int r = sceIoRead(fd, buf + got, size - got);
            if (r <= 0) break;
            got += r;
        }
        sceIoClose(fd);
        buf[got] = 0;
        if (found) snprintf(found, found_len, "%s", path);
        *out = buf;
        return got;
    }
    return -1;
}

static int load_scene(void)
{
    char path[256];
    int size = load_file("scene.ashm", &g_file, path, sizeof(path));
    if (size < (int)sizeof(AshmHeader)) { printf("ash-viewer: scene.ashm not found\n"); return -1; }
    g_hdr = (AshmHeader *)g_file;
    if (memcmp(g_hdr->magic, "ASHM", 4) != 0 || g_hdr->version != 1) {
        printf("ash-viewer: bad header in %s\n", path); return -1;
    }
    g_tex = (AshmTex *)(g_file + g_hdr->off_tex);
    g_batch = (AshmBatch *)(g_file + g_hdr->off_batch);
    g_vert = (AshmVert *)(g_file + g_hdr->off_vert);
    g_idx = (u16 *)(g_file + g_hdr->off_idx);
    g_cam = (AshmCam *)(g_file + g_hdr->off_cam);
    sceKernelDcacheWritebackAll();
    printf("ash-viewer: loaded %s: %d B, tex=%u batches=%u verts=%u tris=%u cams=%u\n", path, size,
           (unsigned)g_hdr->n_tex, (unsigned)g_hdr->n_batch, (unsigned)g_hdr->n_vert,
           (unsigned)(g_hdr->n_idx / 3), (unsigned)g_hdr->n_cam);
    return 0;
}

static int read_camera_index(void)
{
    u8 *buf = NULL;
    if (load_file("camera.txt", &buf, NULL, 0) <= 0) return 0;
    int c = atoi((const char *)buf);
    free(buf);
    return c;
}

/* ---------------------------------------------------------------- frustum culling */
static float g_planes[6][4];

static void extract_planes(const ScePspFMatrix4 *m)
{
    /* clip = M * v (column vectors), rows of M: r0..r3. Planes: r3 +- r0, r3 +- r1, r3 +- r2 */
    float r[4][4];
    const float *a = (const float *)m; /* column-major: a[col*4+row] */
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) r[row][col] = a[col * 4 + row];
    for (int i = 0; i < 3; i++)
        for (int s = 0; s < 2; s++) {
            float *p = g_planes[i * 2 + s];
            float sign = s ? -1.0f : 1.0f;
            for (int k = 0; k < 4; k++) p[k] = r[3][k] + sign * r[i][k];
            float n = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (n > 0) for (int k = 0; k < 4; k++) p[k] /= n;
        }
}

static int sphere_visible(float x, float y, float z, float rad)
{
    for (int i = 0; i < 6; i++) {
        const float *p = g_planes[i];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < -rad) return 0;
    }
    return 1;
}

static void mat_mul(ScePspFMatrix4 *out, const ScePspFMatrix4 *a, const ScePspFMatrix4 *b)
{
    const float *A = (const float *)a, *B = (const float *)b;
    float *O = (float *)out;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += A[k * 4 + r] * B[c * 4 + k];
            O[c * 4 + r] = s;
        }
}

/* ---------------------------------------------------------------- main */
int main(void)
{
    setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    printf("ash-viewer: start, free mem=%u KB, max block=%u KB\n",
           (unsigned)(sceKernelTotalFreeMemSize() / 1024), (unsigned)(sceKernelMaxFreeMemSize() / 1024));
    if (load_scene() < 0) { sceKernelExitGame(); return 0; }
    int cam = read_camera_index();
    if (g_hdr->n_cam == 0) { printf("ash-viewer: no cameras\n"); sceKernelExitGame(); return 0; }
    if (cam < 0 || cam >= (int)g_hdr->n_cam) cam = 0;
    printf("ash-viewer: camera %d, after load free=%u KB max block=%u KB\n", cam,
           (unsigned)(sceKernelTotalFreeMemSize() / 1024), (unsigned)(sceKernelMaxFreeMemSize() / 1024));

    void *fbp0 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *fbp1 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *zbp = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_4444);

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

    pspDebugScreenInitEx((void *)(0x44000000 | (u32)fbp0), PSP_DISPLAY_PIXEL_FORMAT_565, 0);
    pspDebugScreenEnableBackColor(0);

    void *draw = fbp0;
    u64 t_prev, t_now, t_frame0, t_frame1;
    sceRtcGetCurrentTick(&t_prev);
    u32 tick_res = sceRtcGetTickResolution();
    unsigned int frame = 0, frames_in_sec = 0;
    float fps = 0.0f, ge_ms = 0.0f;
    unsigned int drawn_tris = 0, drawn_batches = 0;
    SceCtrlData pad;
    unsigned int prev_buttons = 0;
    const u32 fog_color = 0xff10141a; /* ABGR: very dark blue-grey */

    while (g_running) {
        const AshmCam *c = &g_cam[cam];
        sceRtcGetCurrentTick(&t_frame0);
        sceGuStart(GU_DIRECT, g_list);
        sceGuClearColor(fog_color);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

        sceGuFog(30.0f, 110.0f, fog_color);
        sceGuEnable(GU_FOG);

        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadIdentity();
        sceGumPerspective(c->fov, 480.0f / 272.0f, 0.3f, 400.0f);
        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        {
            ScePspFVector3 eye = {c->eye[0], c->eye[1], c->eye[2]};
            ScePspFVector3 at = {c->target[0], c->target[1], c->target[2]};
            ScePspFVector3 up = {0.0f, 1.0f, 0.0f};
            sceGumLookAt(&eye, &at, &up);
        }
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        {
            ScePspFMatrix4 proj, view, vp;
            sceGumMatrixMode(GU_PROJECTION); sceGumStoreMatrix(&proj);
            sceGumMatrixMode(GU_VIEW); sceGumStoreMatrix(&view);
            sceGumMatrixMode(GU_MODEL);
            mat_mul(&vp, &proj, &view);
            extract_planes(&vp);
        }

        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_REPEAT, GU_REPEAT);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        sceGuAlphaFunc(GU_GREATER, 0x80, 0xff);

        int cur_tex = -1, cur_alpha = -1, cur_cull = -1;
        drawn_tris = drawn_batches = 0;
        for (unsigned int i = 0; i < g_hdr->n_batch; i++) {
            const AshmBatch *b = &g_batch[i];
            if (!sphere_visible(b->cx, b->cy, b->cz, b->radius)) continue;
            if (b->tex != cur_tex) {
                const AshmTex *t = &g_tex[b->tex];
                sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
                sceGuClutLoad(256 / 8, g_file + t->off_clut);
                sceGuTexMode(GU_PSM_T8, 0, 0, 1);
                sceGuTexImage(0, t->w, t->h, t->w, g_file + t->off_pix);
                sceGuTexFlush();
                cur_tex = b->tex;
            }
            int alpha = (b->flags & BF_ALPHA) ? 1 : 0;
            if (alpha != cur_alpha) {
                if (alpha) sceGuEnable(GU_ALPHA_TEST); else sceGuDisable(GU_ALPHA_TEST);
                cur_alpha = alpha;
            }
            int cull = (b->flags & BF_NOCULL) ? 0 : 1;
            if (cull != cur_cull) {
                if (cull) sceGuEnable(GU_CULL_FACE); else sceGuDisable(GU_CULL_FACE);
                cur_cull = cull;
            }
            sceGumDrawArray(GU_TRIANGLES,
                            GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
                            b->idx_count, g_idx + b->idx_start, g_vert + b->vert_start);
            drawn_tris += b->idx_count / 3;
            drawn_batches++;
        }
        sceGuDisable(GU_FOG);
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        sceRtcGetCurrentTick(&t_frame1);
        ge_ms = ge_ms * 0.9f + 0.1f * (float)(t_frame1 - t_frame0) * 1000.0f / (float)tick_res;

        pspDebugScreenSetBase((u32 *)(0x44000000 | (u32)draw));
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenSetTextColor(0xff9adfff);
        pspDebugScreenPrintf("ASH %4.1f fps  cam %d  %u tri  %u dc", fps, cam, drawn_tris, drawn_batches);

        sceDisplayWaitVblankStart();
        draw = sceGuSwapBuffers();

        frame++;
        frames_in_sec++;
        sceRtcGetCurrentTick(&t_now);
        if (t_now - t_prev >= tick_res) {
            fps = (float)frames_in_sec * (float)tick_res / (float)(t_now - t_prev);
            printf("ash-viewer: frame=%u fps=%.1f cam=%d tris=%u batches=%u cpu+ge=%.1fms\n", frame, fps, cam,
                   drawn_tris, drawn_batches, ge_ms);
            frames_in_sec = 0;
            t_prev = t_now;
        }

        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~prev_buttons;
        prev_buttons = pad.Buttons;
        if (pressed & PSP_CTRL_RTRIGGER) cam = (cam + 1) % (int)g_hdr->n_cam;
        if (pressed & PSP_CTRL_LTRIGGER) cam = (cam + (int)g_hdr->n_cam - 1) % (int)g_hdr->n_cam;
        if (pad.Buttons & PSP_CTRL_START) g_running = 0;
    }

    printf("ash-viewer: exit after %u frames\n", frame);
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
