/* Ash AI host — see aihost.h and docs/ai-runtime.md.
 *
 * Lua 5.0.3 (stock, `typedef unsigned int Instruction` on LP64 hosts) runs the original DSR bytecode. Everything the
 * scripts call on the engine side lives here. Semantics we could not read from data are marked [версия]:
 *   - goal tree: a goal ticks its own Update (unless REGISTER_GOAL_NO_UPDATE), then its first sub-goal; a failed
 *     sub-goal fails the parent; when the last sub-goal succeeds the parent succeeds; a Lua goal that never had
 *     sub-goals and has no Update succeeds right after Activate; life <= 0 ends native goals (Success, except
 *     CommonAttack: Failed) — Lua goals with Update read GetLife() themselves (approach_target.lua does);
 *   - think: when the top goal (GOAL_COMMON_TopGoal) has no children the logic function runs (it calls AddTopGoal);
 *   - interrupts: offered top-down along the active chain (skipping NO_INTERUPT goals); first `true` wins and the
 *     goals below it are dropped.
 */
#include "aihost.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#define MAX_GOAL_DEFS 1024
#define MAX_UNKNOWN 128

enum { GF_NO_UPDATE = 1, GF_NO_INTERUPT = 2, GF_UPDATE_TIME = 4, GF_REGISTERED = 8, GF_RESOLVED = 16, GF_COMBO_CANCEL = 32 };

typedef struct {
    int id;
    char name[64];
    int flags;
    int fn[4]; /* Activate, Update, Terminate, Interupt: registry refs, LUA_NOREF if none */
} GoalDef;

struct AiHost {
    lua_State *L;
    GoalDef defs[MAX_GOAL_DEFS];
    int ndefs;
    AiGoal pool[AI_MAX_GOALS];
    AiGoal *free_list;
    int goals_live, goals_peak;
    AiAgent *cur;               /* agent whose Lua code is running */
    const char *unk_name[MAX_UNKNOWN];
    int unk_count[MAX_UNKNOWN];
    int nunk;
    char logic_fn[2][64];       /* last REGISTER_LOGIC_FUNC */
    AiStats st;
    uint64_t (*clock)(void);
    /* well-known goal ids, read from goal_list.lua globals after loading */
    int g_top, g_common_attack, g_wait, g_stay, g_move_to, g_sideway, g_leave, g_keep, g_spin, g_guard, g_back_home,
        g_approach_step, g_parry, g_turn_around;
};

/* ------------------------------------------------------------------ small helpers */

static double getglobal_num(lua_State *L, const char *name, double def) {
    double v = def;
    lua_pushstring(L, name);
    lua_gettable(L, LUA_GLOBALSINDEX);
    if (lua_isnumber(L, -1)) v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

static void logf_(AiHost *h, AiAgent *a, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void logf_(AiHost *h, AiAgent *a, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    (void)h;
    if (a && a->world && a->world->log) a->world->log(a, buf);
}

static GoalDef *def_find(AiHost *h, int id, int create) {
    for (int i = 0; i < h->ndefs; i++)
        if (h->defs[i].id == id) return &h->defs[i];
    if (!create || h->ndefs >= MAX_GOAL_DEFS) return NULL;
    GoalDef *d = &h->defs[h->ndefs++];
    memset(d, 0, sizeof *d);
    d->id = id;
    for (int k = 0; k < 4; k++) d->fn[k] = LUA_NOREF;
    snprintf(d->name, sizeof d->name, "goal%d", id);
    return d;
}

static void def_resolve(AiHost *h, GoalDef *d) {
    static const char *suf[4] = {"_Activate", "_Update", "_Terminate", "_Interupt"};
    if (d->flags & GF_RESOLVED) return;
    d->flags |= GF_RESOLVED;
    if (!(d->flags & GF_REGISTERED)) return;
    lua_State *L = h->L;
    for (int k = 0; k < 4; k++) {
        char n[96];
        snprintf(n, sizeof n, "%s%s", d->name, suf[k]);
        lua_pushstring(L, n);
        lua_gettable(L, LUA_GLOBALSINDEX);
        if (lua_isfunction(L, -1))
            d->fn[k] = luaL_ref(L, LUA_REGISTRYINDEX);
        else
            lua_pop(L, 1);
    }
}

const char *ai_goal_name(AiHost *h, int id) {
    GoalDef *d = def_find(h, id, 0);
    return d ? d->name : "?";
}

static int is_native(AiHost *h, int id) {
    GoalDef *d = def_find(h, id, 0);
    if (!d || id == h->g_common_attack) return 1; /* CommonAttack: Lua has empty stubs, the engine does the work */
    def_resolve(h, d);
    return d->fn[0] == LUA_NOREF && d->fn[1] == LUA_NOREF;
}

static uint32_t rng_next(AiAgent *a) { /* xorshift32 */
    uint32_t x = a->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    a->rng_calls++;
    return a->rng = x;
}

/* ------------------------------------------------------------------ goal objects */

static AiGoal *goal_alloc(AiHost *h) {
    AiGoal *g = h->free_list;
    if (!g) {
        fprintf(stderr, "aihost: goal pool exhausted\n");
        abort();
    }
    h->free_list = g->next;
    memset(g, 0, sizeof *g);
    for (int i = 0; i < AI_MAX_PARAMS; i++) g->param_ref[i] = LUA_NOREF;
    g->obj_ref = LUA_NOREF;
    if (++h->goals_live > h->goals_peak) h->goals_peak = h->goals_live;
    return g;
}

static void push_goal_obj(AiHost *h, AiGoal *g);

static void goal_free_tree(AiHost *h, AiAgent *a, AiGoal *g, int call_terminate);

static void call_fn(AiHost *h, AiAgent *a, int ref, AiGoal *g, int nres, float dt) {
    lua_State *L = h->L;
    AiAgent *prev = h->cur;
    h->cur = a;
    uint64_t t0 = h->clock ? h->clock() : 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, a->obj_ref);
    int nargs = 1;
    if (g) {
        push_goal_obj(h, g);
        nargs++;
        if (dt >= 0) {
            lua_pushnumber(L, dt);
            nargs++;
        }
    }
    if (lua_pcall(L, nargs, nres, 0) != 0) {
        logf_(h, a, "LUA ERROR: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        for (int i = 0; i < nres; i++) lua_pushnil(L);
    }
    if (h->clock) {
        uint64_t dt_us = h->clock() - t0;
        if (!prev) { /* outermost call only */
            h->st.lua_us += dt_us;
            if (dt_us > h->st.lua_us_max) h->st.lua_us_max = (uint32_t)dt_us;
        }
    }
    h->st.lua_calls++;
    h->cur = prev;
}

static void goal_free_tree(AiHost *h, AiAgent *a, AiGoal *g, int call_terminate) {
    while (g->child) {
        AiGoal *c = g->child;
        g->child = c->next;
        goal_free_tree(h, a, c, call_terminate);
    }
    if (call_terminate && g->activated) {
        GoalDef *d = def_find(h, g->id, 0);
        if (d) {
            def_resolve(h, d);
            if (d->fn[2] != LUA_NOREF) call_fn(h, a, d->fn[2], g, 0, -1);
        }
    }
    lua_State *L = h->L;
    for (int i = 0; i < AI_MAX_PARAMS; i++)
        if (g->param_ref[i] != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, g->param_ref[i]);
    if (g->obj_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, g->obj_ref);
    g->next = h->free_list;
    h->free_list = g;
    h->goals_live--;
}

static void clear_children(AiHost *h, AiAgent *a, AiGoal *g) {
    while (g->child) {
        AiGoal *c = g->child;
        g->child = c->next;
        goal_free_tree(h, a, c, 1);
    }
}

/* Lua stack: args from index `first`: id, life, params... -> new child of parent */
static AiGoal *add_goal_from_lua(AiHost *h, AiAgent *a, AiGoal *parent, int first, int front) {
    lua_State *L = h->L;
    int top = lua_gettop(L);
    AiGoal *g = goal_alloc(h);
    g->id = (int)lua_tonumber(L, first);
    g->life0 = g->life = (top >= first + 1) ? (float)lua_tonumber(L, first + 1) : -1.f;
    g->nparams = 0;
    for (int i = first + 2; i <= top && g->nparams < AI_MAX_PARAMS; i++) {
        int k = g->nparams++;
        int t = lua_type(L, i);
        if (t == LUA_TNUMBER) {
            g->param_type[k] = 1;
            g->param[k] = lua_tonumber(L, i);
        } else if (t == LUA_TBOOLEAN) {
            g->param_type[k] = 2;
            g->param[k] = lua_toboolean(L, i);
        } else if (t == LUA_TNIL) {
            g->param_type[k] = 0;
        } else {
            g->param_type[k] = 3;
            lua_pushvalue(L, i);
            g->param_ref[k] = luaL_ref(L, LUA_REGISTRYINDEX);
        }
    }
    g->parent = parent;
    g->depth = parent ? parent->depth + 1 : 0;
    if (front || !parent->child) {
        g->next = parent->child;
        parent->child = g;
    } else {
        AiGoal *c = parent->child;
        while (c->next) c = c->next;
        c->next = g;
    }
    parent->had_children = 1;
    char ps[160] = "";
    int n = 0;
    for (int i = 0; i < g->nparams && n < (int)sizeof ps - 16; i++)
        n += snprintf(ps + n, sizeof ps - n, i ? ",%g" : "%g", g->param_type[i] == 3 ? NAN : g->param[i]);
    logf_(h, a, "%*s+ %s(%d) life=%g [%s]%s", 2 * g->depth, "", ai_goal_name(h, g->id), g->id, g->life0, ps,
          is_native(h, g->id) ? "  native" : "");
    return g;
}

/* ------------------------------------------------------------------ Lua-side objects */

static AiHost *g_host; /* one host per process is enough for the prototype and the PSP */

static void *self_ptr(lua_State *L) {
    void *p = NULL;
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "_p");
        lua_rawget(L, 1);
        p = lua_touserdata(L, -1);
        lua_pop(L, 1);
    }
    return p;
}

#define GOAL_SELF AiGoal *g = (AiGoal *)self_ptr(L); AiAgent *a = g_host->cur; (void)a; if (!g) return 0;
#define AI_SELF AiAgent *a = (AiAgent *)self_ptr(L); if (!a) return 0;

static void push_param(lua_State *L, AiGoal *g, int i) {
    if (i < 0 || i >= g->nparams || g->param_type[i] == 0) lua_pushnil(L);
    else if (g->param_type[i] == 1) lua_pushnumber(L, g->param[i]);
    else if (g->param_type[i] == 2) lua_pushboolean(L, (int)g->param[i]);
    else lua_rawgeti(L, LUA_REGISTRYINDEX, g->param_ref[i]);
}

static int gm_AddSubGoal(lua_State *L) {
    GOAL_SELF
    AiGoal *c = add_goal_from_lua(g_host, a, g, 2, 0);
    push_goal_obj(g_host, c);
    return 1;
}
static int gm_AddSubGoal_Front(lua_State *L) {
    GOAL_SELF
    AiGoal *c = add_goal_from_lua(g_host, a, g, 2, 1);
    push_goal_obj(g_host, c);
    return 1;
}
static int gm_ClearSubGoal(lua_State *L) {
    GOAL_SELF
    clear_children(g_host, a, g);
    return 0;
}
static int gm_GetParam(lua_State *L) {
    GOAL_SELF
    push_param(L, g, (int)luaL_checknumber(L, 2));
    return 1;
}
static int gm_IsExistParam(lua_State *L) {
    GOAL_SELF
    int i = (int)luaL_checknumber(L, 2);
    lua_pushboolean(L, i >= 0 && i < g->nparams && g->param_type[i] != 0);
    return 1;
}
static int gm_GetLife(lua_State *L) {
    GOAL_SELF
    lua_pushnumber(L, g->life0 < 0 ? 1e9 : g->life); /* [версия] endless goal: large positive */
    return 1;
}
static int gm_GetLastSubGoalResult(lua_State *L) {
    GOAL_SELF
    lua_pushnumber(L, g->last_sub_result);
    return 1;
}
static int gm_GetSubGoalNum(lua_State *L) {
    GOAL_SELF
    int n = 0;
    for (AiGoal *c = g->child; c; c = c->next) n++;
    lua_pushnumber(L, n);
    return 1;
}
static int gm_SetNumber(lua_State *L) {
    GOAL_SELF
    int i = (int)luaL_checknumber(L, 2);
    if (i >= 0 && i < 16) g->number[i] = luaL_checknumber(L, 3);
    return 0;
}
static int gm_GetNumber(lua_State *L) {
    GOAL_SELF
    int i = (int)luaL_checknumber(L, 2);
    lua_pushnumber(L, (i >= 0 && i < 16) ? g->number[i] : 0);
    return 1;
}
static int gm_SetTimer(lua_State *L) {
    GOAL_SELF
    int i = (int)luaL_checknumber(L, 2);
    if (i >= 0 && i < 4) g->timer[i] = (float)luaL_checknumber(L, 3);
    return 0;
}

static const luaL_reg goal_methods[] = {
    {"AddSubGoal", gm_AddSubGoal}, {"AddSubGoal_Front", gm_AddSubGoal_Front},
    {"AddLifeParentSubGoal", gm_AddSubGoal}, /* [версия] same as AddSubGoal */
    {"ClearSubGoal", gm_ClearSubGoal}, {"GetParam", gm_GetParam}, {"IsExistParam", gm_IsExistParam},
    {"GetLife", gm_GetLife}, {"GetLastSubGoalResult", gm_GetLastSubGoalResult}, {"GetSubGoalNum", gm_GetSubGoalNum},
    {"SetNumber", gm_SetNumber}, {"GetNumber", gm_GetNumber}, {"SetTimer", gm_SetTimer}, {NULL, NULL}};

static int am_GetDist(lua_State *L) {
    AI_SELF
    lua_pushnumber(L, a->world->get_dist(a, (int)luaL_optnumber(L, 2, 0)));
    return 1;
}
static int am_GetRandam_Int(lua_State *L) {
    AI_SELF
    int lo = (int)luaL_checknumber(L, 2), hi = (int)luaL_checknumber(L, 3);
    int v = hi <= lo ? lo : lo + (int)(rng_next(a) % (uint32_t)(hi - lo + 1));
    lua_pushnumber(L, v);
    return 1;
}
static int am_GetRandam_Float(lua_State *L) {
    AI_SELF
    double lo = luaL_checknumber(L, 2), hi = luaL_checknumber(L, 3);
    lua_pushnumber(L, lo + (hi - lo) * (rng_next(a) >> 8) * (1.0 / 16777216.0));
    return 1;
}
static int am_GetExcelParam(lua_State *L) {
    AI_SELF
    int n = (int)luaL_checknumber(L, 2);
    lua_pushnumber(L, (n >= 0 && n < AI_MAX_EXCEL) ? a->excel[n] : 0);
    return 1;
}
static int am_AddTopGoal(lua_State *L) {
    AI_SELF
    AiGoal *c = add_goal_from_lua(g_host, a, a->root, 2, 0);
    push_goal_obj(g_host, c);
    return 1;
}
static int am_IsInterupt(lua_State *L) {
    AI_SELF
    lua_pushboolean(L, a->interrupt >= 0 && a->interrupt == (int)luaL_checknumber(L, 2));
    return 1;
}
static int am_false(lua_State *L) { lua_pushboolean(L, 0); return 1; }
static int am_zero(lua_State *L) { lua_pushnumber(L, 0); return 1; }
static int am_minus1(lua_State *L) { lua_pushnumber(L, -1); return 1; }
static int am_noop(lua_State *L) { (void)L; return 0; }
static int am_IsBattleState(lua_State *L) {
    AI_SELF
    lua_pushboolean(L, a->battle_state == 1);
    return 1;
}
static int am_IsFindState(lua_State *L) {
    AI_SELF
    lua_pushboolean(L, a->battle_state == 0);
    return 1;
}
static int am_IsChangeState(lua_State *L) {
    AI_SELF
    lua_pushboolean(L, a->battle_state != a->prev_battle_state);
    return 1;
}
static int am_IsSearchTarget(lua_State *L) {
    AI_SELF
    lua_pushboolean(L, a->battle_state == 1);
    return 1;
}
static int am_IsLookToTarget(lua_State *L) {
    AI_SELF
    /* two forms in the data: IsLookToTarget(target, angle) and IsLookToTarget(angle) after TurnTo(target)
       (turn.lua). Points (POINT_*, >= 100) go to get_angle as well; the world decides. */
    int t;
    double ang;
    if (lua_gettop(L) >= 3) {
        t = (int)luaL_checknumber(L, 2);
        ang = luaL_optnumber(L, 3, 90);
    } else {
        t = a->turn_target;
        ang = luaL_optnumber(L, 2, 90);
    }
    lua_pushboolean(L, fabsf(a->world->get_angle(a, t)) <= ang * 0.5); /* [версия] angle = full cone */
    return 1;
}
static int am_GetEventRequest(lua_State *L) {
    AI_SELF
    lua_pushnumber(L, a->event_request);
    return 1;
}
static int am_GetNumber(lua_State *L) {
    AI_SELF
    int i = (int)luaL_checknumber(L, 2);
    lua_pushnumber(L, (i >= 0 && i < AI_MAX_NUMBERS) ? a->number[i] : 0);
    return 1;
}
static int am_SetNumber(lua_State *L) {
    AI_SELF
    int i = (int)luaL_checknumber(L, 2);
    if (i >= 0 && i < AI_MAX_NUMBERS) a->number[i] = luaL_checknumber(L, 3);
    return 0;
}
static int am_SetTimer(lua_State *L) {
    AI_SELF
    int i = (int)luaL_checknumber(L, 2);
    if (i >= 0 && i < AI_MAX_TIMERS) a->timer[i] = (float)luaL_checknumber(L, 3);
    return 0;
}
static int am_GetTimer(lua_State *L) {
    AI_SELF
    int i = (int)luaL_checknumber(L, 2);
    lua_pushnumber(L, (i >= 0 && i < AI_MAX_TIMERS) ? a->timer[i] : 0);
    return 1;
}
static int am_IsFinishTimer(lua_State *L) {
    AI_SELF
    int i = (int)luaL_checknumber(L, 2);
    lua_pushboolean(L, (i >= 0 && i < AI_MAX_TIMERS) ? a->timer[i] <= 0 : 1);
    return 1;
}
static int am_HasTopSubgoal(lua_State *L) {
    AI_SELF
    lua_pushboolean(L, a->root->child != NULL);
    return 1;
}
static int am_DoEzAction(lua_State *L) {
    AI_SELF
    int ez = (int)luaL_checknumber(L, 3);
    logf_(g_host, a, "  ai:DoEzAction(%g, %d)", luaL_checknumber(L, 2), ez);
    return 0;
}
static int am_PrintText(lua_State *L) {
    AI_SELF
    logf_(g_host, a, "  PrintText: %s", lua_tostring(L, 2) ? lua_tostring(L, 2) : "?");
    return 0;
}
static int am_TurnTo(lua_State *L) {
    AI_SELF
    a->turn_target = (int)luaL_optnumber(L, 2, 0);
    a->move_target = a->turn_target;
    a->world->move(a, 0, 0, 1, 1, a->dt);
    return 0;
}
/* optional world callbacks (AiWorld fields may be NULL -> arena defaults) */
static int am_IsInsideTargetRegion(lua_State *L) {
    AI_SELF
    int t = (int)luaL_checknumber(L, 2), region = (int)luaL_checknumber(L, 3);
    lua_pushboolean(L, a->world->in_region ? a->world->in_region(a, t, region) : 0);
    return 1;
}
static int am_AddObserveChrDmySphere(lua_State *L) {
    AI_SELF
    int slot = (int)luaL_checknumber(L, 2);
    if (slot >= 0 && slot < AI_MAX_OBSERVE) {
        a->observe[slot].active = 1;
        a->observe[slot].target = (int)luaL_optnumber(L, 3, 0);
        a->observe[slot].dmy = (int)luaL_optnumber(L, 4, -1);
        a->observe[slot].radius = (float)luaL_optnumber(L, 5, 1);
    }
    return 0;
}
static int am_IsInsideObserve(lua_State *L) {
    AI_SELF
    int slot = (int)luaL_checknumber(L, 2), in = 0;
    if (slot >= 0 && slot < AI_MAX_OBSERVE && a->observe[slot].active && a->world->dmy_sphere)
        in = a->world->dmy_sphere(a, a->observe[slot].dmy, a->observe[slot].radius, a->observe[slot].target);
    lua_pushboolean(L, in);
    return 1;
}
static int am_GetDistYSigned(lua_State *L) {
    AI_SELF
    lua_pushnumber(L, a->world->get_dist_y ? a->world->get_dist_y(a, (int)luaL_optnumber(L, 2, 0)) : 0);
    return 1;
}
static int am_GetDistY(lua_State *L) {
    AI_SELF
    lua_pushnumber(L, a->world->get_dist_y ? fabsf(a->world->get_dist_y(a, (int)luaL_optnumber(L, 2, 0))) : 0);
    return 1;
}
static int am_SetEventMoveTarget(lua_State *L) {
    AI_SELF
    a->event_move_target = (int)luaL_checknumber(L, 2);
    if (a->world->set_event_target) a->world->set_event_target(a, a->event_move_target);
    return 0;
}

static const luaL_reg ai_methods[] = {
    {"GetDist", am_GetDist}, {"GetRandam_Int", am_GetRandam_Int}, {"GetRandam_Float", am_GetRandam_Float},
    {"GetExcelParam", am_GetExcelParam}, {"AddTopGoal", am_AddTopGoal}, {"IsInterupt", am_IsInterupt},
    {"IsBattleState", am_IsBattleState}, {"IsCautionState", am_false}, {"IsFindState", am_IsFindState},
    {"IsChangeState", am_IsChangeState}, {"IsSearchTarget", am_IsSearchTarget}, {"IsLookToTarget", am_IsLookToTarget},
    {"GetEventRequest", am_GetEventRequest}, {"GetNumber", am_GetNumber}, {"SetNumber", am_SetNumber},
    {"SetTimer", am_SetTimer}, {"GetTimer", am_GetTimer}, {"IsFinishTimer", am_IsFinishTimer},
    {"HasTopSubgoal", am_HasTopSubgoal}, {"DoEzAction", am_DoEzAction}, {"PrintText", am_PrintText},
    {"TurnTo", am_TurnTo},
    /* arena: no platoon, no team, no ladders, no breakables, no forced/replanned goals, no debug forcing */
    {"ReqPlatoonState", am_noop}, {"TeamHelp_IsValidCall", am_false}, {"TeamHelp_IsValidReply", am_false},
    {"TeamHelp_ValidateCall", am_noop}, {"TeamHelp_ValidateReply", am_noop}, {"TeamHelp_Call", am_noop},
    {"TeamHelp_Reply", am_noop}, {"TeamHelp_ReserveCall", am_noop}, {"GetTeamOrder", am_minus1},
    {"GetTeamRecordCount", am_zero}, {"IsLadderAct", am_false}, {"IsTouchBreakableObject", am_false},
    {"IsForceBattleGoal", am_false}, {"ClearForceBattleGoal", am_noop}, {"GetReplanningGoalAction", am_zero},
    {"GetReplanningGoalID", am_minus1}, {"GetMovePointEffectRange", am_zero}, {"GetMovePointNumber", am_minus1},
    {"DbgGetForceActIdx", am_minus1}, {"DbgSetLastActIdx", am_noop}, {"SetAttackRequest", am_noop},
    {"IsFinishAttack", am_false}, {"IsStartAttack", am_false},
    /* Asylum Demon 223200 (api_223200.txt) extras, arena stubs [версия]: no MSB regions, no observe spheres */
    {"IsInsideTargetRegion", am_IsInsideTargetRegion}, {"IsInsideObserve", am_IsInsideObserve},
    {"AddObserveChrDmySphere", am_AddObserveChrDmySphere}, {"GetDistYSigned", am_GetDistYSigned},
    {"GetDistY", am_GetDistY}, {"SetEventMoveTarget", am_SetEventMoveTarget},
    {NULL, NULL}};

/* __index for ai/goal tables: known method -> it; unknown -> a logging stub returning nil (and counted) */
static int unknown_method(lua_State *L) {
    const char *nm = lua_tostring(L, lua_upvalueindex(1));
    AiHost *h = g_host;
    int i;
    for (i = 0; i < h->nunk; i++)
        if (!strcmp(h->unk_name[i], nm)) break;
    if (i == h->nunk && h->nunk < MAX_UNKNOWN) {
        h->unk_name[i] = strdup(nm);
        h->unk_count[i] = 0;
        h->nunk++;
    }
    if (i < h->nunk) h->unk_count[i]++;
    if (h->cur) logf_(h, h->cur, "  ?? unimplemented %s", nm);
    return 0;
}

static int obj_index(lua_State *L) {
    /* upvalue 1: method table, upvalue 2: prefix string */
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    lua_pushfstring(L, "%s:%s", lua_tostring(L, lua_upvalueindex(2)), lua_tostring(L, 2));
    lua_pushcclosure(L, unknown_method, 1);
    return 1;
}

static int make_meta(lua_State *L, const luaL_reg *methods, const char *prefix) {
    lua_newtable(L); /* metatable */
    lua_pushstring(L, "__index");
    lua_newtable(L);
    for (const luaL_reg *r = methods; r->name; r++) {
        lua_pushstring(L, r->name);
        lua_pushcfunction(L, r->func);
        lua_rawset(L, -3);
    }
    lua_pushstring(L, prefix);
    lua_pushcclosure(L, obj_index, 2);
    lua_rawset(L, -3);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

static int g_goal_meta = LUA_NOREF, g_ai_meta = LUA_NOREF;

static int new_obj(lua_State *L, void *p, int meta) {
    lua_newtable(L);
    lua_pushstring(L, "_p");
    lua_pushlightuserdata(L, p);
    lua_rawset(L, -3);
    lua_rawgeti(L, LUA_REGISTRYINDEX, meta);
    lua_setmetatable(L, -2);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

static void push_goal_obj(AiHost *h, AiGoal *g) {
    if (g->obj_ref == LUA_NOREF) g->obj_ref = new_obj(h->L, g, g_goal_meta);
    lua_rawgeti(h->L, LUA_REGISTRYINDEX, g->obj_ref);
}

/* ------------------------------------------------------------------ REGISTER_* globals */

static int r_goal(lua_State *L) {
    GoalDef *d = def_find(g_host, (int)luaL_checknumber(L, 1), 1);
    if (d) {
        snprintf(d->name, sizeof d->name, "%s", luaL_checkstring(L, 2));
        d->flags |= GF_REGISTERED;
        d->flags &= ~GF_RESOLVED;
    }
    return 0;
}
static int r_flag(lua_State *L, int f) {
    GoalDef *d = def_find(g_host, (int)luaL_checknumber(L, 1), 1);
    int on = lua_gettop(L) < 2 || lua_toboolean(L, 2) || (lua_isnumber(L, 2) && lua_tonumber(L, 2) != 0);
    if (d) d->flags = on ? (d->flags | f) : (d->flags & ~f);
    return 0;
}
static int r_no_update(lua_State *L) { return r_flag(L, GF_NO_UPDATE); }
static int r_no_interupt(lua_State *L) { return r_flag(L, GF_NO_INTERUPT); }
static int r_combo_cancel(lua_State *L) { /* ENABLE_COMBO_ATK_CANCEL(goal): combo goals may cut the EzState */
    GoalDef *d = def_find(g_host, (int)luaL_checknumber(L, 1), 1);
    if (d) d->flags |= GF_COMBO_CANCEL;
    return 0;
}
static int r_update_time(lua_State *L) {
    GoalDef *d = def_find(g_host, (int)luaL_checknumber(L, 1), 1);
    if (d) d->flags |= GF_UPDATE_TIME;
    return 0;
}
static int r_logic(lua_State *L) {
    snprintf(g_host->logic_fn[0], 64, "%s", luaL_optstring(L, 2, ""));
    snprintf(g_host->logic_fn[1], 64, "%s", luaL_optstring(L, 3, ""));
    return 0;
}

/* REGIST_FUNC replacement. The original (common_battle_func.lua:830) is
 *   function REGIST_FUNC(ai, goal, f, p) return function() return f(ai, goal, p) end end
 * and Common_Battle_Activate then does `if GetRandam_Int(1,100) <= <act's return> then AdjustSpace end` (line 133).
 * Some original acts return nothing: 223200 (Asylum Demon) Act15 and Act16 ("go home" when the player leaves
 * region 1812990) end with a bare RETURN 0 1 after `GetWellSpace_Odds = 0` [проверено: 223200_battle.lst]. On stock
 * Lua 5.0 that raises "attempt to compare number with nil" on every such think. The error comes after all
 * sub-goals are pushed, so the only thing it skips is AdjustSpace - the same as odds 0. Here a nil return becomes 0:
 * identical behaviour, no error. Other return values pass through unchanged (first value only - the caller uses
 * one). Installed after each chunk load, because common_battle_func defines the Lua version as a global. */
static int regist_call(lua_State *L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_pushvalue(L, lua_upvalueindex(3));
    lua_pushvalue(L, lua_upvalueindex(4));
    lua_call(L, 3, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushnumber(L, 0);
    }
    return 1;
}
static int r_regist_func(lua_State *L) {
    lua_settop(L, 4);               /* ai, goal, func, paramTbl (paramTbl may be nil) */
    lua_pushvalue(L, 3);            /* upvalue order: func, ai, goal, paramTbl */
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 4);
    lua_pushcclosure(L, regist_call, 4);
    return 1;
}

/* ------------------------------------------------------------------ host */

AiHost *ai_host_new(void) {
    AiHost *h = calloc(1, sizeof *h);
    g_host = h;
    lua_State *L = h->L = lua_open();
    luaopen_base(L);
    luaopen_table(L);
    luaopen_string(L);
    luaopen_math(L);
    lua_settop(L, 0);
    for (int i = AI_MAX_GOALS - 1; i >= 0; i--) {
        h->pool[i].next = h->free_list;
        h->free_list = &h->pool[i];
    }
    g_goal_meta = make_meta(L, goal_methods, "goal");
    g_ai_meta = make_meta(L, ai_methods, "ai");
    lua_register(L, "REGISTER_GOAL", r_goal);
    lua_register(L, "REGISTER_GOAL_NO_UPDATE", r_no_update);
    lua_register(L, "REGISTER_GOAL_NO_INTERUPT", r_no_interupt);
    lua_register(L, "REGISTER_GOAL_UPDATE_TIME", r_update_time);
    lua_register(L, "REGISTER_DBG_GOAL_PARAM", am_noop);
    lua_register(L, "REGISTER_LOGIC_FUNC", r_logic);
    lua_register(L, "ENABLE_COMBO_ATK_CANCEL", r_combo_cancel);
    return h;
}

void ai_host_free(AiHost *h) {
    lua_close(h->L);
    for (int i = 0; i < h->nunk; i++) free((void *)h->unk_name[i]);
    free(h);
}

int ai_host_load(AiHost *h, const char *path) {
    lua_State *L = h->L;
    if (luaL_loadfile(L, path) || lua_pcall(L, 0, 0, 0)) {
        fprintf(stderr, "aihost: %s: %s\n", path, lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    for (int i = 0; i < h->ndefs; i++) h->defs[i].flags &= ~GF_RESOLVED;
    lua_register(L, "REGIST_FUNC", r_regist_func); /* override the script's own, see r_regist_func */
    return 0;
}

void ai_host_set_clock(AiHost *h, uint64_t (*clock_us)(void)) { h->clock = clock_us; }
const AiStats *ai_host_stats(AiHost *h) {
    h->st.goals_peak = h->goals_peak;
    h->st.lua_kb = lua_getgccount(h->L);
    return &h->st;
}

int ai_host_unknown(AiHost *h, const char ***names, const int **counts) {
    *names = h->unk_name;
    *counts = h->unk_count;
    return h->nunk;
}

static int fn_ref(lua_State *L, const char *name) {
    lua_pushstring(L, name);
    lua_gettable(L, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return LUA_NOREF;
    }
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

int ai_agent_init(AiHost *h, AiAgent *a, const AiWorld *w, const char *logic_fn, const char *logic_int_fn,
                  uint32_t seed) {
    lua_State *L = h->L;
    a->world = w;
    a->rng = seed ? seed : 0x12345678u;
    a->interrupt = -1;
    a->obj_ref = new_obj(L, a, g_ai_meta);
    a->logic_ref = fn_ref(L, logic_fn);
    a->logic_int_ref = fn_ref(L, logic_int_fn);
    if (a->logic_ref == LUA_NOREF) {
        fprintf(stderr, "aihost: logic function %s not loaded\n", logic_fn);
        return -1;
    }
#define GID(var, name) h->var = (int)getglobal_num(L, name, -1)
    GID(g_top, "GOAL_COMMON_TopGoal");
    GID(g_common_attack, "GOAL_COMMON_CommonAttack");
    GID(g_wait, "GOAL_COMMON_Wait");
    GID(g_stay, "GOAL_COMMON_Stay");
    GID(g_move_to, "GOAL_COMMON_MoveToSomewhere");
    GID(g_sideway, "GOAL_COMMON_SidewayMove");
    GID(g_leave, "GOAL_COMMON_LeaveTarget");
    GID(g_keep, "GOAL_COMMON_KeepDist");
    GID(g_spin, "GOAL_COMMON_SpinStep");
    GID(g_guard, "GOAL_COMMON_Guard");
    GID(g_back_home, "GOAL_COMMON_BackToHome");
    GID(g_approach_step, "GOAL_COMMON_ApproachStep");
    GID(g_parry, "GOAL_COMMON_Parry");
    GID(g_turn_around, "GOAL_COMMON_TurnAround");
#undef GID
    a->root = goal_alloc(h);
    a->root->id = h->g_top;
    a->root->life0 = a->root->life = -1;
    a->root->activated = 1;
    return 0;
}

/* ------------------------------------------------------------------ native goals (PC stubs, docs/ai-runtime.md) */

static int native_update(AiHost *h, AiAgent *a, AiGoal *g, float dt) {
    const AiWorld *w = a->world;
    int id = g->id;
    /* the goal's target parameter: CommonAttack/SpinStep (ez, target, ...), movement goals (target, ...) */
    int tgt = 0;
    if (id == h->g_common_attack || id == h->g_spin) tgt = g->nparams > 1 ? (int)g->param[1] : 0;
    else if (g->nparams > 0 && g->param_type[0] == 1) tgt = (int)g->param[0];
    if (id == h->g_wait || id == h->g_stay || id == h->g_guard) tgt = 0;
    a->move_target = tgt; /* AiWorld.move / get_angle act relative to this target (TARGET_ENE_0 or POINT_*) */
    float d = w->get_dist(a, tgt);
    int first = g->t == 0;
    int expired = g->life0 >= 0 && g->life <= 0;
    if (id == h->g_common_attack) {
        /* (life, ezState, target, distType, successAngle, turnTime, turnFaceAngle, isCombo, isMoveCancel,
           isGuardBreak, isTurn, angleUp, angleDown). [версия]: face the target up to turnTime, then play the
           EzState; Success when it ends. Range checks against distType are left to the real engine. */
        if (first) {
            g->nat_i = 0;
            g->nat_a = (float)(g->nparams > 4 ? g->param[4] : 1.5); /* turnTime */
        }
        if (g->nat_i == 0) {
            float ang = fabsf(w->get_angle(a, tgt));
            float face = (float)(g->nparams > 5 ? g->param[5] : 20);
            if (ang > face && g->t < g->nat_a) {
                w->move(a, 0, 0, 1, 1, dt);
                return AI_GOAL_CONTINUE;
            }
            g->nat_i = 1;
            a->ez_state = (int)g->param[0];
            a->ez_left = w->ez_action(a, a->ez_state);
            logf_(h, a, "%*s>> ATTACK ezState=%d  dist=%.2f  combo=%d", 2 * g->depth, "", a->ez_state, d,
                  (int)(g->nparams > 6 ? g->param[6] : 0));
        }
        a->ez_left -= dt;
        if (a->ez_left <= 0) return AI_GOAL_SUCCESS;
        return expired ? AI_GOAL_FAILED : AI_GOAL_CONTINUE;
    }
    if (id == h->g_spin) { /* (life, anim 700..703, target, ?, dirType, dist) */
        if (first) {
            a->ez_state = (int)g->param[0];
            a->ez_left = w->ez_action(a, a->ez_state);
            logf_(h, a, "%*s>> STEP ezState=%d  dist=%.2f", 2 * g->depth, "", a->ez_state, d);
        }
        a->ez_left -= dt;
        return a->ez_left <= 0 ? AI_GOAL_SUCCESS : AI_GOAL_CONTINUE;
    }
    if (id == h->g_move_to) { /* (life, target, dirType, range, turnTarget, walk) */
        float range = (float)(g->nparams > 2 ? g->param[2] : 1);
        int walk = g->nparams > 4 ? g->param[4] != 0 : 1;
        if (d <= range) return AI_GOAL_SUCCESS;
        if (expired) return AI_GOAL_SUCCESS; /* [версия] */
        w->move(a, 1, 0, walk, 1, dt);
        return AI_GOAL_CONTINUE;
    }
    if (id == h->g_leave) { /* (life, target, dist, turnTarget, walk, guardEz) */
        float want = (float)(g->nparams > 1 ? g->param[1] : 3);
        if (d >= want || expired) return AI_GOAL_SUCCESS;
        w->move(a, -1, 0, 1, 1, dt);
        return AI_GOAL_CONTINUE;
    }
    if (id == h->g_keep) { /* (life, target, min, max, turnTarget, walk, guardEz) */
        float mn = (float)g->param[1], mx = (float)g->param[2];
        if ((d >= mn && d <= mx) || expired) return AI_GOAL_SUCCESS;
        w->move(a, d > mx ? 1 : -1, 0, 1, 1, dt);
        return AI_GOAL_CONTINUE;
    }
    if (id == h->g_sideway) { /* (life, target, right, angle, turnTo, walk, guardEz) */
        if (first) g->nat_a = 0;
        float side = g->param[1] != 0 ? 1.f : -1.f;
        w->move(a, 0, side, 1, 1, dt);
        g->nat_a += dt * 1.5f / (d > 0.5f ? d : 0.5f) * 57.29578f; /* walked arc, degrees */
        if (g->nat_a >= (float)g->param[2] || expired) return AI_GOAL_SUCCESS;
        return AI_GOAL_CONTINUE;
    }
    if (id == h->g_wait || id == h->g_stay || id == h->g_guard) {
        if (g->life0 < 0) return AI_GOAL_SUCCESS; /* [версия] endless Wait would hang the stub world */
        return expired ? AI_GOAL_SUCCESS : AI_GOAL_CONTINUE;
    }
    /* anything else (TurnAround, BackToHome, Parry, ...) is not needed by the hollow: finish at once, loudly */
    if (first) logf_(h, a, "%*s?? native goal %s(%d) not implemented -> Success", 2 * g->depth, "",
                     ai_goal_name(h, id), id);
    return AI_GOAL_SUCCESS;
}

/* ------------------------------------------------------------------ tick */

static int update_goal(AiHost *h, AiAgent *a, AiGoal *g, float dt) {
    GoalDef *d = def_find(h, g->id, 0);
    if (d) def_resolve(h, d);
    int native = is_native(h, g->id);
    if (!g->activated) {
        g->activated = 1;
        if (!native && d->fn[0] != LUA_NOREF) call_fn(h, a, d->fn[0], g, 0, -1);
    }
    if (g->life0 >= 0) g->life -= dt;
    int r = AI_GOAL_CONTINUE;
    if (native) {
        r = native_update(h, a, g, dt);
        g->t += dt;
        return r;
    }
    if (!(d->flags & GF_NO_UPDATE) && d->fn[1] != LUA_NOREF) {
        call_fn(h, a, d->fn[1], g, 1, dt);
        r = lua_isnumber(h->L, -1) ? (int)lua_tonumber(h->L, -1) : AI_GOAL_CONTINUE;
        lua_pop(h->L, 1);
        if (r != AI_GOAL_CONTINUE) {
            g->t += dt;
            return r;
        }
    }
    g->t += dt;
    if (g->child) {
        AiGoal *c = g->child;
        int cr = update_goal(h, a, c, dt);
        if (cr != AI_GOAL_CONTINUE) {
            g->last_sub_result = cr;
            g->child = c->next;
            goal_free_tree(h, a, c, 1);
            if (cr == AI_GOAL_FAILED) {
                clear_children(h, a, g);
                return AI_GOAL_FAILED;
            }
            if (!g->child) return AI_GOAL_SUCCESS;
        }
        return AI_GOAL_CONTINUE;
    }
    if (!g->had_children && ((d->flags & GF_NO_UPDATE) || d->fn[1] == LUA_NOREF)) return AI_GOAL_SUCCESS;
    if (g->had_children) return AI_GOAL_SUCCESS;
    return AI_GOAL_CONTINUE;
}

void ai_agent_tick(AiHost *h, AiAgent *a, float dt) {
    a->now += dt;
    a->dt = dt;
    for (int i = 0; i < AI_MAX_TIMERS; i++) a->timer[i] -= dt;
    if (!a->root->child) {
        a->think_count++;
        logf_(h, a, "-- think #%d  dist=%.2f  rng_calls=%lu", a->think_count, a->world->get_dist(a, 0),
              (unsigned long)a->rng_calls);
        call_fn(h, a, a->logic_ref, NULL, 0, -1);
        a->prev_battle_state = a->battle_state;
        if (!a->root->child) return;
    }
    AiGoal *c = a->root->child;
    int r = update_goal(h, a, c, dt);
    if (r != AI_GOAL_CONTINUE) {
        logf_(h, a, "%*s- %s(%d) -> %s", 2 * c->depth, "", ai_goal_name(h, c->id), c->id,
              r == AI_GOAL_SUCCESS ? "Success" : "Failed");
        a->root->child = c->next;
        goal_free_tree(h, a, c, 1);
    }
}

int ai_agent_interrupt(AiHost *h, AiAgent *a, int interupt) {
    a->interrupt = interupt;
    int taken = 0;
    for (AiGoal *g = a->root->child; g && !taken; g = g->child) {
        GoalDef *d = def_find(h, g->id, 0);
        if (!d) continue;
        def_resolve(h, d);
        if ((d->flags & GF_NO_INTERUPT) || d->fn[3] == LUA_NOREF) continue;
        call_fn(h, a, d->fn[3], g, 1, -1);
        taken = lua_toboolean(h->L, -1);
        lua_pop(h->L, 1);
        if (taken) logf_(h, a, "%*s! interrupt %d taken by %s", 2 * g->depth, "", interupt, d->name);
    }
    if (!taken && a->logic_int_ref != LUA_NOREF) {
        call_fn(h, a, a->logic_int_ref, a->root, 1, -1);
        lua_pop(h->L, 1);
    }
    a->interrupt = -1;
    return taken;
}

void ai_agent_free(AiHost *h, AiAgent *a) {
    if (a->root) goal_free_tree(h, a, a->root, 0);
    a->root = NULL;
    lua_State *L = h->L;
    if (a->obj_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, a->obj_ref);
    if (a->logic_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, a->logic_ref);
    if (a->logic_int_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, a->logic_int_ref);
    a->obj_ref = a->logic_ref = a->logic_int_ref = LUA_NOREF;
}
