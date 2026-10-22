/* Ash save file: see save.h. */
#include <stdio.h>
#include <string.h>

#include "save.h"

uint32_t save_crc32(const void *data, int len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xffffffffu;
    for (int i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static int al4(int n) { return (n + 3) & ~3; }

int save_packed_size(const SavePart *parts, int n)
{
    int s = 12 + 8 * n;
    for (int i = 0; i < n; i++) s += al4(parts[i].size);
    return s + 4;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

int save_pack(void *buf, int cap, const SavePart *parts, int n)
{
    if (n < 0 || n > SAVE_MAX_PARTS) return -1;
    int total = save_packed_size(parts, n);
    if (total > cap) return -1;
    uint8_t *b = (uint8_t *)buf;
    memset(b, 0, total);
    memcpy(b, "ASHS", 4);
    put32(b + 4, 1);
    put32(b + 8, (uint32_t)n);
    int o = 12 + 8 * n;
    for (int i = 0; i < n; i++) {
        put32(b + 12 + 8 * i, parts[i].tag);
        put32(b + 16 + 8 * i, (uint32_t)parts[i].size);
        if (parts[i].size > 0) memcpy(b + o, parts[i].data, parts[i].size);
        o += al4(parts[i].size);
    }
    put32(b + o, save_crc32(b, o));
    return o + 4;
}

int save_unpack(const void *buf, int len, SavePart *parts, int max_parts)
{
    const uint8_t *b = (const uint8_t *)buf;
    if (len < 16) return -1;
    if (memcmp(b, "ASHS", 4) != 0 || get32(b + 4) != 1) return -2;
    uint32_t n = get32(b + 8);
    if (n > SAVE_MAX_PARTS || 12 + 8 * (int)n + 4 > len) return -1;
    int o = 12 + 8 * (int)n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sz = get32(b + 16 + 8 * i);
        if (sz > (uint32_t)len || o + al4((int)sz) + 4 > len) return -1;
        if ((int)i < max_parts) { parts[i].tag = get32(b + 12 + 8 * i); parts[i].data = b + o; parts[i].size = (int)sz; }
        o += al4((int)sz);
    }
    if (get32(b + o) != save_crc32(b, o)) return -3;
    return (int)n < max_parts ? (int)n : max_parts;
}

const SavePart *save_find(const SavePart *parts, int n, uint32_t tag)
{
    for (int i = 0; i < n; i++) if (parts[i].tag == tag) return &parts[i];
    return 0;
}

/* ---------------------------------------------------------------- file I/O */

#ifdef __psp__
#include <pspiofilemgr.h>
#include <malloc.h>

int save_write_file(const char *path, const SavePart *parts, int n)
{
    int size = save_packed_size(parts, n);
    uint8_t *buf = memalign(64, size);
    if (!buf) return -1;
    int len = save_pack(buf, size, parts, n);
    if (len < 0) { free(buf); return -2; }
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    SceUID fd = sceIoOpen(tmp, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { free(buf); return -3; }
    int w = sceIoWrite(fd, buf, len);
    sceIoClose(fd);
    free(buf);
    if (w != len) { sceIoRemove(tmp); return -4; }
    sceIoRemove(path);
    if (sceIoRename(tmp, path) < 0) {
        /* some IO drivers have no rename: write in place (still CRC-protected) */
        fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd < 0) return -5;
        buf = memalign(64, size);
        if (!buf) { sceIoClose(fd); return -1; }
        save_pack(buf, size, parts, n);
        w = sceIoWrite(fd, buf, len);
        sceIoClose(fd);
        free(buf);
        sceIoRemove(tmp);
        if (w != len) return -4;
    }
    return 0;
}

int save_read_file(const char *path, void *buf, int cap, SavePart *parts, int max_parts)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return -10;
    int got = sceIoRead(fd, buf, cap);
    sceIoClose(fd);
    if (got <= 0) return -1;
    return save_unpack(buf, got, parts, max_parts);
}
#else
#include <stdio.h>
#include <stdlib.h>

int save_write_file(const char *path, const SavePart *parts, int n)
{
    int size = save_packed_size(parts, n);
    uint8_t *buf = malloc(size);
    if (!buf) return -1;
    int len = save_pack(buf, size, parts, n);
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f || len < 0) { free(buf); if (f) fclose(f); return -3; }
    int w = (int)fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);
    if (w != len) { remove(tmp); return -4; }
    remove(path);
    return rename(tmp, path) == 0 ? 0 : -5;
}

int save_read_file(const char *path, void *buf, int cap, SavePart *parts, int max_parts)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -10;
    int got = (int)fread(buf, 1, cap, f);
    fclose(f);
    if (got <= 0) return -1;
    return save_unpack(buf, got, parts, max_parts);
}
#endif
