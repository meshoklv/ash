/*
 * Ash save file: one file of tagged parts (progress blob, event-flags blob, ...) + CRC-32, written atomically
 * (temp file, then rename). Pure packing in save_pack/save_unpack (tested natively); file I/O via sceIo on the PSP,
 * stdio elsewhere.
 *
 * Where: next to the EBOOT (the game's cwd: ms0:/PSP/GAME/ASH/ash.sav on a memory stick, the staging dir under
 * PPSSPPHeadless). Not sceUtilitySavedata: that one needs the utility dialog loop and PARAM.SFO; the file is enough
 * for the S1 slice (the decisions design notes / docs/progress-ui.md) [придумал].
 *
 * Layout v1 (little-endian): "ASHS", u32 version=1, u32 n_parts, n x {u32 tag, u32 size}, part data (each 4-aligned),
 * u32 crc32 (IEEE, of everything before it).
 */
#ifndef ASH_SAVE_H
#define ASH_SAVE_H

#include <stdint.h>

#define SAVE_PATH "ash.sav"
#define SAVE_MAX_PARTS 8
#define SAVE_TAG(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define SAVE_TAG_PROGRESS SAVE_TAG('P', 'R', 'G', '1')
#define SAVE_TAG_EVENTS SAVE_TAG('E', 'V', 'T', '1')

typedef struct { uint32_t tag; const void *data; int size; } SavePart;

uint32_t save_crc32(const void *data, int len);
int save_packed_size(const SavePart *parts, int n);
int save_pack(void *buf, int cap, const SavePart *parts, int n);            /* bytes or <0 */
/* parts[i].data points into buf. Returns the number of parts, <0: -1 short, -2 magic/version, -3 crc */
int save_unpack(const void *buf, int len, SavePart *parts, int max_parts);
const SavePart *save_find(const SavePart *parts, int n, uint32_t tag);

int save_write_file(const char *path, const SavePart *parts, int n);       /* 0 ok */
/* Reads the whole file into buf (cap bytes) and unpacks it. Returns parts or <0 (-10 no file). */
int save_read_file(const char *path, void *buf, int cap, SavePart *parts, int max_parts);

#endif
