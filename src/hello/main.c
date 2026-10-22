/*
 * Ash M0 hello: sceGu clear + rotating textured cube + FPS overlay.
 * Exit: START (or HOME menu -> exit).
 * Prints "ash-hello: fps=..." to stdout once per second (visible in PPSSPPHeadless log).
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
#include <string.h>

PSP_MODULE_INFO("Ash Hello", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272
#define TEX_SIZE 64

static unsigned int __attribute__((aligned(16))) g_list[64 * 1024];
static unsigned int __attribute__((aligned(16))) g_tex[TEX_SIZE * TEX_SIZE];

typedef struct {
    float u, v;
    unsigned int color;
    float x, y, z;
} Vertex;

/* 6 faces x 2 triangles, CW winding as seen from outside (matches pspsdk cube sample). */
static Vertex __attribute__((aligned(16))) g_cube[36] = {
    {0,0,0xffffffff,-1,-1, 1},{1,0,0xffffffff,-1, 1, 1},{1,1,0xffffffff, 1, 1, 1},
    {0,0,0xffffffff,-1,-1, 1},{1,1,0xffffffff, 1, 1, 1},{0,1,0xffffffff, 1,-1, 1},
    {0,0,0xffffffff,-1,-1,-1},{1,0,0xffffffff, 1,-1,-1},{1,1,0xffffffff, 1, 1,-1},
    {0,0,0xffffffff,-1,-1,-1},{1,1,0xffffffff, 1, 1,-1},{0,1,0xffffffff,-1, 1,-1},
    {0,0,0xff80ffff, 1,-1,-1},{1,0,0xff80ffff, 1,-1, 1},{1,1,0xff80ffff, 1, 1, 1},
    {0,0,0xff80ffff, 1,-1,-1},{1,1,0xff80ffff, 1, 1, 1},{0,1,0xff80ffff, 1, 1,-1},
    {0,0,0xff80ffff,-1,-1,-1},{1,0,0xff80ffff,-1, 1,-1},{1,1,0xff80ffff,-1, 1, 1},
    {0,0,0xff80ffff,-1,-1,-1},{1,1,0xff80ffff,-1, 1, 1},{0,1,0xff80ffff,-1,-1, 1},
    {0,0,0xffff80ff,-1, 1,-1},{1,0,0xffff80ff, 1, 1,-1},{1,1,0xffff80ff, 1, 1, 1},
    {0,0,0xffff80ff,-1, 1,-1},{1,1,0xffff80ff, 1, 1, 1},{0,1,0xffff80ff,-1, 1, 1},
    {0,0,0xffff80ff,-1,-1,-1},{1,0,0xffff80ff,-1,-1, 1},{1,1,0xffff80ff, 1,-1, 1},
    {0,0,0xffff80ff,-1,-1,-1},{1,1,0xffff80ff, 1,-1, 1},{0,1,0xffff80ff, 1,-1,-1},
};

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

/* Procedural 8x8 checker with an orange "ember" border: no external assets. ABGR8888. */
static void make_texture(void)
{
    for (int y = 0; y < TEX_SIZE; y++)
        for (int x = 0; x < TEX_SIZE; x++) {
            unsigned int c = (((x >> 3) ^ (y >> 3)) & 1) ? 0xff303030 : 0xffd0d0d0;
            if (x < 3 || y < 3 || x >= TEX_SIZE - 3 || y >= TEX_SIZE - 3) c = 0xff1080ff;
            g_tex[y * TEX_SIZE + x] = c;
        }
    sceKernelDcacheWritebackRange(g_tex, sizeof(g_tex));
}

int main(void)
{
    setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    make_texture();

    /* PSP_HEAP_SIZE_KB(-1024): newlib heap takes all user RAM but 1 MB, so kernel-free is small by design. */
    printf("ash-hello: start, kernel free after heap=%u KB (max block %u KB)\n",
           (unsigned)(sceKernelTotalFreeMemSize() / 1024), (unsigned)(sceKernelMaxFreeMemSize() / 1024));

    void *fbp0 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_8888);
    void *fbp1 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_8888);
    void *zbp  = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_4444);

    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_8888, fbp0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, fbp1, BUF_WIDTH);
    sceGuDepthBuffer(zbp, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CULL_FACE);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    /* Debug text is drawn straight into the current draw buffer after the GE finishes. */
    pspDebugScreenInitEx((void *)(0x44000000 | (u32)fbp0), PSP_DISPLAY_PIXEL_FORMAT_8888, 0);
    pspDebugScreenEnableBackColor(0);

    void *draw = fbp0;
    u64 t_prev, t_now;
    sceRtcGetCurrentTick(&t_prev);
    u32 tick_res = sceRtcGetTickResolution();
    unsigned int frame = 0, frames_in_sec = 0;
    float fps = 0.0f;
    SceCtrlData pad;

    while (g_running) {
        sceGuStart(GU_DIRECT, g_list);
        sceGuClearColor(0xff402010); /* ABGR: dark blue */
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadIdentity();
        sceGumPerspective(75.0f, 16.0f / 9.0f, 0.5f, 1000.0f);
        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        {
            ScePspFVector3 pos = {0.0f, 0.0f, -3.0f};
            ScePspFVector3 rot = {frame * 0.79f * (GU_PI / 180.0f),
                                  frame * 0.98f * (GU_PI / 180.0f),
                                  frame * 1.32f * (GU_PI / 180.0f)};
            sceGumTranslate(&pos);
            sceGumRotateXYZ(&rot);
        }

        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, TEX_SIZE, TEX_SIZE, TEX_SIZE, g_tex);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);

        sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                        36, 0, g_cube);

        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

        /* FPS overlay into the buffer we just rendered */
        pspDebugScreenSetBase((u32 *)(0x44000000 | (u32)draw));
        pspDebugScreenSetXY(1, 1);
        pspDebugScreenSetTextColor(0xffffffff);
        pspDebugScreenPrintf("ASH M0  FPS: %5.1f  frame: %u", fps, frame);
        pspDebugScreenSetXY(1, 32);
        pspDebugScreenPrintf("START = exit");

        sceDisplayWaitVblankStart();
        draw = sceGuSwapBuffers();

        frame++;
        frames_in_sec++;
        sceRtcGetCurrentTick(&t_now);
        if (t_now - t_prev >= tick_res) {
            fps = (float)frames_in_sec * (float)tick_res / (float)(t_now - t_prev);
            printf("ash-hello: frame=%u fps=%.1f\n", frame, fps);
            frames_in_sec = 0;
            t_prev = t_now;
        }

        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_START) g_running = 0;
    }

    printf("ash-hello: exit after %u frames\n", frame);
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
