/* Ash save storage: sceUtilitySavedata A/B + file with tmp/rename (savestore.h). */
#include <stdio.h>
#include <string.h>

#include "savestore.h"

/* step kinds */
enum { STEP_LOAD_A, STEP_LOAD_B, STEP_LOAD_FILE, STEP_SAVE_UTIL, STEP_SAVE_FILE };
enum { OP_NONE, OP_READ, OP_WRITE };

static uint8_t io_buf[SG_MAX_BYTES] __attribute__((aligned(64)));     /* utility data buffer / packed write */
static uint8_t best_buf[SG_MAX_BYTES] __attribute__((aligned(64)));   /* packed bytes of the newest valid copy */
static int best_len;
static SaveGame cand;                                                 /* validation of each copy read */
static SaveGame pending;                                              /* the game being written */

const char *sv_src_name(int src)
{
    static const char *const n[SV_SRC_COUNT] = {"none", "savedata A", "savedata B", "file"};
    return src >= 0 && src < SV_SRC_COUNT ? n[src] : "?";
}

void sv_init(SaveStore *st, int backends, const char *file_path)
{
    memset(st, 0, sizeof(*st));
#ifndef __psp__
    backends &= ~SV_BACKEND_UTILITY;
#endif
    st->backends = backends;
    snprintf(st->file_path, sizeof(st->file_path), "%s", file_path ? file_path : SV_FILE_PATH);
    for (int i = 0; i < SV_SRC_COUNT; i++) st->src_result[i] = -101;
}

int sv_busy(const SaveStore *st) { return st->op != OP_NONE; }

static int use_util(const SaveStore *st) { return (st->backends & SV_BACKEND_UTILITY) && !st->util_failed; }

/* a copy has been read into io_buf[0..len): validate, keep if newest */
static void consider(SaveStore *st, int src, int len)
{
    int r = len > 0 ? sg_unpack(&cand, io_buf, len) : len;
    st->src_result[src] = r;
    st->src_seq[src] = r == 0 ? cand.seq : 0;
    if (r == 0 && (st->newest_src == SV_SRC_NONE || cand.seq > st->newest_seq)) {
        st->newest_src = src;
        st->newest_seq = cand.seq;
        memcpy(best_buf, io_buf, len);
        best_len = len;
    }
}

/* ================================================================= file backend */

#ifdef __psp__
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspdisplay.h>
#include <psputility.h>
#include <psputility_savedata.h>
#include <psputility_sysparam.h>
#include <psprtc.h>

static int file_read(const char *path, uint8_t *buf, int cap)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return -101;
    int got = sceIoRead(fd, buf, cap);
    sceIoClose(fd);
    return got > 0 ? got : -1;
}

static int file_write(const char *path, const uint8_t *buf, int len)
{
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    SceUID fd = sceIoOpen(tmp, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -3;
    int w = sceIoWrite(fd, buf, len);
    sceIoClose(fd);
    if (w != len) { sceIoRemove(tmp); return -4; }
    /* fatms rename does not replace an existing file: remove the old one first. A cut between the remove and the
       rename leaves only <path>.tmp (complete) - load_file falls back to it. */
    sceIoRemove(path);
    if (sceIoRename(tmp, path) < 0) {
        fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);   /* no rename: in place (CRC) */
        if (fd < 0) return -5;
        w = sceIoWrite(fd, buf, len);
        sceIoClose(fd);
        sceIoRemove(tmp);
        if (w != len) return -4;
    }
    return 0;
}
#else
static int file_read(const char *path, uint8_t *buf, int cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -101;
    int got = (int)fread(buf, 1, cap, f);
    fclose(f);
    return got > 0 ? got : -1;
}

static int file_write(const char *path, const uint8_t *buf, int len)
{
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -3;
    int w = (int)fwrite(buf, 1, len, f);
    if (fclose(f) != 0) w = -1;
    if (w != len) { remove(tmp); return -4; }
    return rename(tmp, path) == 0 ? 0 : -5;   /* POSIX rename replaces atomically */
}
#endif

static void load_file(SaveStore *st)
{
    int len = file_read(st->file_path, io_buf, sizeof(io_buf));
    if (len == -101) {   /* cut between remove and rename (PSP): the finished temp file is still there */
        char tmp[160];
        snprintf(tmp, sizeof(tmp), "%s.tmp", st->file_path);
        len = file_read(tmp, io_buf, sizeof(io_buf));
    }
    consider(st, SV_SRC_FILE, len);
}

/* ================================================================= sceUtilitySavedata */

#ifdef __psp__
#include "icon0.h"   /* ICON0.PNG 144x80 (src/save/mkicon.py) */

static SceUtilitySavedataParam up __attribute__((aligned(64)));
static int up_src;
static int up_polls;   /* polls of the running utility operation (timeout below) */
#define UTIL_MAX_POLLS 1200   /* ~20-40 s: a stuck utility -> error + file fallback instead of a frozen game */

static int util_start(SaveStore *st, int save, int src, int len)
{
    memset(&up, 0, sizeof(up));
    up.base.size = sizeof(up);
    int lang = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH, swap = 1;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &lang);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &swap);   /* X/O confirm */
    up.base.language = lang;
    up.base.buttonSwap = swap;
    up.base.graphicsThread = 0x11;   /* the usual values of homebrew samples [версия] */
    up.base.accessThread = 0x13;
    up.base.fontThread = 0x12;
    up.base.soundThread = 0x10;
    up.mode = save ? PSP_UTILITY_SAVEDATA_AUTOSAVE : PSP_UTILITY_SAVEDATA_AUTOLOAD;
    up.overwrite = 1;
    strcpy(up.gameName, SV_GAME_NAME);
    strcpy(up.saveName, src == SV_SRC_UTIL_A ? "A" : "B");
    strcpy(up.fileName, "DATA.BIN");
    /* 1536-byte request (FW >= 2.00 layout): secureVersion 0 needs a game key - without one the hardware answers
       SCE_UTILITY_SAVEDATA_ERROR_SAVE_PARAM 0x80110388 (PPSSPP SavedataParam.cpp MissingRequiredKey; seen in our
       first emulator run). Our own constant key: the data is encrypted with it, so it must never change. */
    memcpy(up.key, SV_KEY, 16);
    up.secureVersion = 0;
    up.dataBuf = io_buf;
    up.dataBufSize = sizeof(io_buf);
    up.dataSize = save ? (SceSize)len : sizeof(io_buf);
    if (save) {
        const SaveGame *s = &pending;
        snprintf(up.sfoParam.title, sizeof(up.sfoParam.title), "Ash: DS1");
        snprintf(up.sfoParam.savedataTitle, sizeof(up.sfoParam.savedataTitle), "%.100s", s->name[0] ? s->name : "Ash");
        const char *zone = sg_zone_name(s->area, s->block);
        snprintf(up.sfoParam.detail, sizeof(up.sfoParam.detail),
                 "%s\nУровень %d   Души %d\nВремя в игре %u:%02u   (копия %s, №%u)", zone[0] ? zone : "-",
                 s->level, (int)s->souls, (unsigned)(s->play_s / 3600), (unsigned)(s->play_s / 60 % 60),
                 src == SV_SRC_UTIL_A ? "A" : "B", (unsigned)s->seq);
        up.sfoParam.parentalLevel = 1;
        up.icon0FileData.buf = (void *)ash_icon0_png;
        up.icon0FileData.bufSize = sizeof(ash_icon0_png);
        up.icon0FileData.size = sizeof(ash_icon0_png);
    }
    up_src = src;
    up_polls = 0;
    int r = sceUtilitySavedataInitStart(&up);
    if (r < 0) { st->last_err = r; return -1; }
    st->util_running = 1;
    return 0;
}

/* 1 = still running, 0 = finished (up.base.result has the outcome), -1 = timed out.
   FINISHED (4) means "shutting down": on the hardware the next InitStart is refused until the status is back to
   NONE (0), so only NONE ends the operation (docs/hw-pitfalls.md; it used to end on 4 -> the next InitStart failed
   -> util_failed -> every later save went to the file only). */
static int util_poll(void)
{
    if (++up_polls > UTIL_MAX_POLLS) return -1;
    switch (sceUtilitySavedataGetStatus()) {
    case PSP_UTILITY_DIALOG_INIT: return 1;
    case PSP_UTILITY_DIALOG_VISIBLE: sceUtilitySavedataUpdate(1); return 1;
    case PSP_UTILITY_DIALOG_QUIT: sceUtilitySavedataShutdownStart(); return 1;
    case PSP_UTILITY_DIALOG_FINISHED: return 1;   /* still unloading */
    case PSP_UTILITY_DIALOG_NONE: default: return 0;
    }
}
#endif

/* ================================================================= operations */

#ifdef __psp__
static int pick_util_slot(const SaveStore *st)
{
    /* the slot not holding the newest valid copy: an invalid / absent one first, else the older one */
    int a = st->src_result[SV_SRC_UTIL_A] == 0, b = st->src_result[SV_SRC_UTIL_B] == 0;
    if (!a) return SV_SRC_UTIL_A;
    if (!b) return SV_SRC_UTIL_B;
    return st->src_seq[SV_SRC_UTIL_A] <= st->src_seq[SV_SRC_UTIL_B] ? SV_SRC_UTIL_A : SV_SRC_UTIL_B;
}
#endif

static int finish(SaveStore *st, int status)
{
    if (status == SV_OK && st->op == OP_WRITE && st->user) st->user->seq = pending.seq;
    if (status == SV_ERR && st->op == OP_WRITE) st->known = 0;   /* copies unsure after a failed write: re-read */
    st->op = OP_NONE;
    st->status = status;
    return status;
}

/* seq = newest + 1 (the loads queued before this step have run), packed into io_buf */
static int pack_pending(SaveStore *st)
{
    pending.seq = st->newest_seq + 1;
    int len = sg_pack(&pending, io_buf, sizeof(io_buf));
    if (len < 0) { st->last_err = len; return -1; }
    st->write_len = (uint32_t)len;
    return 0;
}

static void written(SaveStore *st, int src)
{
    st->src_result[src] = 0;
    st->src_seq[src] = st->newest_seq = pending.seq;
    st->newest_src = st->last_src = src;
}

/* runs steps until one has to wait for the utility; returns SV_BUSY or the final status */
static int run(SaveStore *st)
{
    st->frames++;
    for (;;) {
#ifdef __psp__
        if (st->util_running) {
            int pr = util_poll();
            if (pr > 0) return SV_BUSY;
            st->util_running = 0;
            int res = up.base.result;
            if (pr < 0) { res = -110; st->util_failed = 1; }   /* stuck utility: stop using it this session */
            if (res != 0) st->last_err = res;
            if (st->steps[st->step] == STEP_SAVE_UTIL) {
                if (res != 0) {
                    if (!(st->backends & SV_BACKEND_FILE)) return finish(st, SV_ERR);
                    st->steps[st->step] = STEP_SAVE_FILE;   /* fall back to the file */
                    continue;
                }
                written(st, up_src);
            } else {
                /* 0x80110307 SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA: this copy does not exist */
                consider(st, up_src, res == 0 ? (int)up.dataSize : (res == (int)0x80110307 ? -101 : -100));
            }
            st->step++;
        }
#endif
        if (st->step >= st->n_steps) {
            if (st->op == OP_READ) {
                st->known = 1;
                st->last_src = st->newest_src;
                if (st->newest_src == SV_SRC_NONE) return finish(st, SV_ERR);
                sg_unpack(st->target, best_buf, best_len);
            }
            return finish(st, SV_OK);
        }
        int kind = st->steps[st->step];
        if (kind == STEP_LOAD_FILE) {
            load_file(st);
            st->step++;
            continue;
        }
        if (kind == STEP_SAVE_FILE) {
            if (pack_pending(st) < 0) return finish(st, SV_ERR);
            int r = file_write(st->file_path, io_buf, (int)st->write_len);
            if (r != 0) { st->last_err = r; return finish(st, SV_ERR); }
            written(st, SV_SRC_FILE);
            st->step++;
            continue;
        }
#ifdef __psp__
        if (kind == STEP_LOAD_A || kind == STEP_LOAD_B) {
            int src = kind == STEP_LOAD_A ? SV_SRC_UTIL_A : SV_SRC_UTIL_B;
            if (st->util_failed || util_start(st, 0, src, 0) < 0) {
                st->util_failed = 1;
                st->src_result[src] = -100;
                st->step++;
                continue;
            }
            return SV_BUSY;
        }
        if (kind == STEP_SAVE_UTIL) {
            if (pack_pending(st) < 0) return finish(st, SV_ERR);
            if (st->util_failed || util_start(st, 1, pick_util_slot(st), (int)st->write_len) < 0) {
                st->util_failed = 1;
                if (!(st->backends & SV_BACKEND_FILE)) return finish(st, SV_ERR);
                st->steps[st->step] = STEP_SAVE_FILE;
                continue;
            }
            return SV_BUSY;
        }
#endif
        st->step++;   /* a utility step without the utility (native build): skip */
    }
}

static void queue_loads(SaveStore *st)
{
    st->newest_src = SV_SRC_NONE;
    st->newest_seq = 0;
    for (int i = 0; i < SV_SRC_COUNT; i++) { st->src_result[i] = -101; st->src_seq[i] = 0; }
    if (use_util(st)) { st->steps[st->n_steps++] = STEP_LOAD_A; st->steps[st->n_steps++] = STEP_LOAD_B; }
    if (st->backends & SV_BACKEND_FILE) st->steps[st->n_steps++] = STEP_LOAD_FILE;
}

int sv_read_begin(SaveStore *st, SaveGame *out)
{
    if (st->op != OP_NONE) return SV_ERR;
    st->op = OP_READ;
    st->target = out;
    st->user = 0;
    st->step = st->n_steps = st->frames = 0;
    queue_loads(st);
    st->status = SV_BUSY;
    return SV_BUSY;
}

int sv_write_begin(SaveStore *st, SaveGame *s)
{
    if (st->op != OP_NONE) return SV_ERR;
    st->op = OP_WRITE;
    st->user = s;
    st->target = 0;
    st->step = st->n_steps = st->frames = 0;
#ifdef __psp__
    u64 tick = 0;
    sceRtcGetCurrentTick(&tick);
    s->time = tick;
#endif
    pending = *s;   /* copied: the game may change s while the utility works */
    /* the first write without a read: find the newest copy first, so it is never the one overwritten */
    if (!st->known) queue_loads(st);
    st->known = 1;
    st->steps[st->n_steps++] = use_util(st) ? STEP_SAVE_UTIL : STEP_SAVE_FILE;
    st->status = SV_BUSY;
    return SV_BUSY;
}

int sv_poll(SaveStore *st)
{
    if (st->op == OP_NONE) {
        int s = st->status;
        st->status = SV_IDLE;
        return s == SV_BUSY ? SV_IDLE : s;
    }
    int r = run(st);
    if (r != SV_BUSY) st->status = SV_IDLE;
    return r;
}

static void wait_frame(void)
{
#ifdef __psp__
    sceDisplayWaitVblankStart();
#endif
}

int sv_write_sync(SaveStore *st, SaveGame *s)
{
    if (sv_write_begin(st, s) == SV_ERR) return SV_ERR;
    int r;
    while ((r = sv_poll(st)) == SV_BUSY) wait_frame();
    return r;
}

int sv_read_sync(SaveStore *st, SaveGame *out)
{
    if (sv_read_begin(st, out) == SV_ERR) return SV_ERR;
    int r;
    while ((r = sv_poll(st)) == SV_BUSY) wait_frame();
    return r;
}
