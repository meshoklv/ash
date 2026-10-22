/*
 * The Asylum Demon driven by FromSoftware's original AI script (223200_battle + aiCommon, Lua 5.0 bytecode on the
 * src/ai host) instead of the C state machine of boss.c. This file only maps the host's AiWorld callbacks onto the
 * Boss body: distances/angles/regions/observe spheres, locomotion, EzState requests.
 *
 *   AiHost *h = ai_host_new();  ...load aiCommon (load_order.txt) + m18_01_00_00/223200_battle.luac...
 *   BossAi ba; boss_ai_attach(&ba, h, &boss, "ai/think_223200.txt", seed);   // boss.ai_mode = BOSS_AI_EXTERNAL
 *   every step: boss_ai_tick(&ba, &world, DT);    // = ai_agent_tick + boss_tick
 *   events:     boss_ai_interrupt(&ba, BOSS_AI_INT_DAMAGED) after boss_take_hit(), ... _FIND_ATTACK, _USE_ITEM
 *
 * Conventions: the host's angles are degrees, + = right; boss.angle is radians, + = left.
 */
#ifndef ASH_BOSS_AI_H
#define ASH_BOSS_AI_H

#include "aihost.h"
#include "boss.h"

/* INTERUPT_* values from ai_define.lua (the ai-facts design notes) */
enum { BOSS_AI_INT_FIND_ATTACK = 1, BOSS_AI_INT_DAMAGED = 2, BOSS_AI_INT_USE_ITEM = 12 };

typedef struct {
    AiHost *host;
    AiAgent agent;
    AiWorld world;
    Boss *boss;
    const BossWorld *bw;     /* world of the current tick */
    float home[3];           /* POINT_INITIAL */
    float event_pt[3];       /* POINT_EVENT (SetEventMoveTarget) */
    int has_event_pt, moved;
} BossAi;

int boss_ai_attach(BossAi *ba, AiHost *h, Boss *b, const char *think_txt, u32 seed);
void boss_ai_tick(BossAi *ba, const BossWorld *w, float dt);
void boss_ai_interrupt(BossAi *ba, int interupt);

#endif
