/*
 * Ash save storage (docs/save.md): where the bytes of sg_pack() go.
 *
 * PSP, primary - sceUtilitySavedata in the no-dialog modes (AUTOSAVE / AUTOLOAD): ms0:/PSP/SAVEDATA/ASHDS0001A and
 *   .../ASHDS0001B, each with DATA.BIN + PARAM.SFO + ICON0.PNG, shown in the XMB as "Ash: DS1" with our icon.
 *   Two copies written in turn (A/B): a write goes to the copy that does NOT hold the newest valid save, so a power
 *   cut or a pulled memory stick during a write can only spoil the copy being written; the load takes the valid
 *   copy (CRC-32 + checks) with the highest seq. The utility has no rename, so this is the tmp+rename of the file
 *   backend done with two slots [придумал].
 * PSP / native, secondary - a plain file (default "ash.sav" next to the EBOOT: ms0:/PSP/GAME/ASH/ash.sav), written
 *   to "<path>.tmp" then renamed over the old one. Used when the utility is unavailable (InitStart fails) and by the
 *   native tests; on load it is read too, so the newest of everything wins.
 *
 * The utility runs asynchronously over several frames: begin an operation, then call sv_poll() once per frame
 * (before or after the frame's GU work - AUTOSAVE / AUTOLOAD draw nothing) until it is not SV_BUSY. The game keeps
 * drawing meanwhile (the "Сохранение..." toast of src/ui). sv_*_sync() loop on sceDisplayWaitVblankStart.
 */
#ifndef ASH_SAVESTORE_H
#define ASH_SAVESTORE_H

#include "savegame.h"

enum { SV_IDLE, SV_BUSY, SV_OK, SV_ERR };

enum {
    SV_BACKEND_UTILITY = 1,    /* sceUtilitySavedata A/B (PSP only; ignored natively) */
    SV_BACKEND_FILE = 2,       /* file + tmp/rename */
    SV_BACKEND_ALL = 3,
};

enum { SV_SRC_NONE, SV_SRC_UTIL_A, SV_SRC_UTIL_B, SV_SRC_FILE, SV_SRC_COUNT };

#define SV_GAME_NAME "ASHDS0001"   /* 9 chars like a disc id; saveName "A" / "B" -> ASHDS0001A / ASHDS0001B */
#define SV_FILE_PATH "ash.sav"
/* savedata encryption key - fixed forever, or old saves stop loading [придумал: 16 bytes of our own] */
#define SV_KEY "AshDS1-PSP-key01"

typedef struct {
    int backends;              /* SV_BACKEND_* mask */
    char file_path[128];
    /* result of the last read / write */
    int newest_src;            /* SV_SRC_* holding the newest valid save (after a read), NONE if none */
    uint32_t newest_seq;
    int known;                 /* newest_src is known (a read ran) */
    int src_result[SV_SRC_COUNT];   /* per source: 0 valid, <0 sg_unpack error, -100 utility error, -101 absent */
    uint32_t src_seq[SV_SRC_COUNT];
    int last_src;              /* source written by the last write / chosen by the last read */
    int last_err;              /* utility base.result or file error of the last failing step */
    int util_failed;           /* InitStart refused: utility not usable, file only from now on */
    /* operation in flight */
    int op, step, n_steps, steps[4], status;
    int util_running;
    SaveGame *target;          /* read: where the result goes */
    SaveGame *user;            /* write: gets .seq on success */
    uint32_t write_len;
    int frames;                /* sv_poll calls of the current operation */
} SaveStore;

void sv_init(SaveStore *st, int backends, const char *file_path);   /* file_path 0 -> SV_FILE_PATH */

/* Start writing s: seq = newest + 1, s->time to the RTC (PSP). s must stay valid until done
   (it is copied at once, so it may change after this call returns; .seq is set on success). Returns SV_BUSY / SV_ERR. */
int sv_write_begin(SaveStore *st, SaveGame *s);
/* Start reading the newest valid save into *out (untouched unless SV_OK). Returns SV_BUSY / SV_ERR. */
int sv_read_begin(SaveStore *st, SaveGame *out);
int sv_poll(SaveStore *st);      /* SV_BUSY, then SV_OK or SV_ERR once (afterwards SV_IDLE) */
int sv_busy(const SaveStore *st);

int sv_write_sync(SaveStore *st, SaveGame *s);     /* SV_OK / SV_ERR */
int sv_read_sync(SaveStore *st, SaveGame *out);    /* SV_OK / SV_ERR (no valid save anywhere) */

const char *sv_src_name(int src);

#endif
