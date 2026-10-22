/* No-op stand-ins for the PSP system calls the game modules reference (native build only, tests/native). */
#include <stdlib.h>
#include <string.h>
static unsigned char g_gu_mem[1 << 20];
static int g_gu_off;
void *sceGuGetMemory(int size) { if (g_gu_off + size > (int)sizeof g_gu_mem) g_gu_off = 0; void *p = g_gu_mem + g_gu_off; g_gu_off += (size + 15) & ~15; return p; }
/* drawing / state: nothing to do without a GPU */
#define STUB(name) void name(void) {}
STUB(sceGuAlphaFunc) STUB(sceGuBlendFunc) STUB(sceGuBoneMatrix) STUB(sceGuClutLoad) STUB(sceGuClutMode)
STUB(sceGuDepthMask) STUB(sceGuDisable) STUB(sceGuDrawArray) STUB(sceGuEnable) STUB(sceGuFog) STUB(sceGuSetMatrix)
STUB(sceGuShadeModel) STUB(sceGuTexFilter) STUB(sceGuTexFlush) STUB(sceGuTexFunc) STUB(sceGuTexImage)
STUB(sceGuTexMode) STUB(sceGuTexOffset) STUB(sceGuTexScale) STUB(sceGuTexWrap) STUB(sceGumDrawArray)
STUB(sceGumLoadIdentity) STUB(sceGumMatrixMode) STUB(sceGumRotateY) STUB(sceGumTranslate)
STUB(sceKernelDcacheWritebackAll) STUB(sceKernelDcacheWritebackRange) STUB(sceGuColor) STUB(sceGuAmbientColor)
/* files go through the native loader (asy_native.c); world_read_file is not used natively */
int sceIoOpen(const char *p, int f, int m) { (void)p; (void)f; (void)m; return -1; }
int sceIoClose(int fd) { (void)fd; return 0; }
long long sceIoLseek(int fd, long long o, int w) { (void)fd; (void)o; (void)w; return 0; }
int sceIoRead(int fd, void *b, int n) { (void)fd; (void)b; (void)n; return 0; }
