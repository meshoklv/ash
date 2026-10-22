/* Internal: GE display list walker + cost model (src/perf). */
#ifndef ASH_GE_SCAN_H
#define ASH_GE_SCAN_H

#include "perf_cost_gen.h"

extern float g_pc[PC__COUNT];
#define PC(k) (g_pc[PC_##k])

/* vertex position formats */
enum { VF_S8 = 0, VF_S16, VF_FLOAT, VF_THROUGH, VF_N };

typedef struct {
    /* counts */
    int lists, cmds, prims, prims_skinned, verts, verts_fmt[VF_N], verts_skinned, verts_lit;
    int tris, tris_culled, tris_clipped, sprites;
    int tex_binds, tex_binds_ram, clut_loads, clut_bytes, bone_mats, max_weights, max_bones_draw;
    int tex_over512, tex_nonclut, tex_unswizzled;
    int unreadable;            /* draws whose vertices could not be read (pixels unknown) */
    double vbytes, ibytes;
    /* pixels (counted with overdraw, before the depth test) */
    double px, px_tex, px_zr, px_zw, px_blend, px_clear, px_tex_ram;
    double tex_bytes_ram, tex_bytes_vram;   /* texel bytes fetched (bpp x texels/pixel, clamped) */
    double minif_max;                       /* worst texels/pixel of a triangle (minification) */
    double tex_ratio_sum;                   /* sum of covered px x min(texels/pixel, 64): / px_tex = average */
    /* cost, ns at 166 MHz bus */
    double ns_list, ns_cmd, ns_vert, ns_pix, ns_state, ns_total;
} GeStats;

void ge_stats_clear(GeStats *s);
void ge_stats_add(GeStats *a, const GeStats *b);
/* Walk the list starting at `list` (CPU pointer), following CALL/JUMP, until END; adds into *s. */
void ge_scan_list(const void *list, int size_bytes, GeStats *s);
/* set when log_draws=1: one line per draw goes to printf */
extern int g_ge_log_draws;
extern int g_ge_coverage;
extern int g_ge_list_seq;

#endif
