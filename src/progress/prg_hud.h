/*
 * Glue Progress -> HudFrame (PSP side). The game sets the rest (stamina, boss, prompt):
 *     hud_frame_clear(&f);
 *     prg_fill_hud(&prog, &ui, &f);          // HP (bound pointers), Estus, souls, banner, toast, fade, menu
 *     f.sp = actor.sp; f.sp_max = actor.sp_max;
 *     f.boss_on = ...; f.prompt = prg_resting(&prog) ? 0 : ev_prompt(ev);
 */
#ifndef ASH_PRG_HUD_H
#define ASH_PRG_HUD_H

#include "progress.h"
#include "hud.h"

void prg_fill_hud(const Progress *p, const UiRes *ui, HudFrame *f);

#endif
