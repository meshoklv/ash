/* Rim pass for `.ashc` characters (src/game/chr.h). Separate header so render.h does not depend on src/game. */
#ifndef ASH_RENDER_CHR_H
#define ASH_RENDER_CHR_H

#include "render.h"
#include "chr.h"

/* Second skinned pass of c with the view-space sphere-map rim (RF_RIM). Call right after chr_draw with the same
   GU_MODEL matrix still loaded. alpha_chunks = 0 skips alpha-tested chunks (capes, hair: their cut-outs would get
   a rim too), 1 includes them (the knight has 16 of 29 chunks flagged alpha, mostly armour). Returns triangles. */
int render_char_rim(const Chr *c, const ScePspFMatrix4 *skin, int alpha_chunks);

#endif
