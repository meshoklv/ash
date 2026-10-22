/*
 * ashexc.prx - a tiny KERNEL module for the game's crash catcher (docs/hw-pitfalls.md, src/diag/diag.c).
 *
 * A user-mode EBOOT on 6.60 cannot register an exception handler (ExceptionManagerForKernel is kernel-only), so the
 * game loads this module with kuKernelLoadModule (KUBridge of PRO-C / ARK) and starts it with three user addresses:
 *   cfg[0] = AshExcRegs block in the game's memory (registers land there),
 *   cfg[1] = park function in the game (user code that writes crash.txt and shows the screen),
 *   cfg[2] = top of a spare stack in the game (the crashed thread's own stack may be the cause).
 * On ANY unhandled CPU exception the handler (ashexc_asm.S) stores the registers + EPC/Cause/BadVAddr/Status into the
 * block and erets into the park function on the spare stack, in the crashed thread, back in user mode.
 * The result of sceKernelRegisterDefaultExceptionHandler is written into the block (word 1) for the game's log.
 * No libc: this module links only kernel stubs.
 */
#include <pspkernel.h>
#include <pspexception.h>

PSP_MODULE_INFO("AshExc", 0x1000, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();

extern void ashexc_handler(void);
unsigned int ashexc_cfg[3];

int module_start(SceSize args, void *argp)
{
    if (args < 12 || !argp) return 0;
    const unsigned int *a = (const unsigned int *)argp;
    for (int i = 0; i < 3; i++) ashexc_cfg[i] = a[i];
    int r = sceKernelRegisterDefaultExceptionHandler((void *)((unsigned int)ashexc_handler | 0x80000000u));
    ((volatile unsigned int *)ashexc_cfg[0])[1] = (unsigned int)r;   /* AshExcRegs.installed */
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    return 0;
}
