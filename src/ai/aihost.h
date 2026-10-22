/* Ash AI host: runs FromSoftware's original DS1 AI Lua 5.0 bytecode (aiCommon + map battle scripts) on a stock
 * Lua 5.0.3 VM and provides the engine side: goal tree, native goals, ai:/goal: methods, REGISTER_* globals.
 *
 * Status: PC prototype (docs/ai-runtime.md). Semantics marked [версия] in aihost.c are our reconstruction, not
 * observed DSR behaviour. No dependency on src/game: the world is reached only through AiWorld callbacks.
 */
#ifndef ASH_AIHOST_H
#define ASH_AIHOST_H

#include <stdint.h>

#define AI_MAX_PARAMS 16
#define AI_MAX_GOALS 256        /* pool shared by all agents of one host */
#define AI_MAX_EXCEL 32
#define AI_MAX_NUMBERS 64
#define AI_MAX_TIMERS 16
#define AI_MAX_OBSERVE 8

enum { AI_GOAL_FAILED = -1, AI_GOAL_CONTINUE = 0, AI_GOAL_SUCCESS = 1 };

typedef struct AiGoal AiGoal;
struct AiGoal {
    int id;
    float life, life0;          /* life0 < 0: endless */
    int nparams;
    double param[AI_MAX_PARAMS];
    uint8_t param_type[AI_MAX_PARAMS]; /* 0 nil, 1 number, 2 bool, 3 other (kept by ref) */
    int param_ref[AI_MAX_PARAMS];
    int obj_ref;                /* Lua table passed as `goal` */
    AiGoal *parent, *child, *next;
    int activated, had_children, depth;
    int last_sub_result;
    float t;                    /* time since activation */
    float timer[4];
    double number[16];
    /* native-goal state */
    float nat_a, nat_b;
    int nat_i;
};

typedef struct AiAgent AiAgent;

/* Engine callbacks. All distances in metres, angles in degrees. Target 0 = TARGET_ENE_0, -1 = TARGET_SELF. */
typedef struct {
    float (*get_dist)(AiAgent *a, int target);
    /* signed angle from the agent's forward to the target (TARGET_* or POINT_*), degrees, + = right [версия] */
    float (*get_angle)(AiAgent *a, int target);
    /* native locomotion relative to a->move_target (set by the host before the call; TARGET_* or POINT_*):
       forward (-1 back, 0 stop, 1 towards), side (-1 left, +1 right), walk (else run), face the target */
    void (*move)(AiAgent *a, float forward, float side, int walk, int face_target, float dt);
    /* request an EzState action (attack 3000.., step 700..): returns its length in seconds */
    float (*ez_action)(AiAgent *a, int ez_state);
    void (*log)(AiAgent *a, const char *line);
    /* ---- optional (NULL = arena default in brackets). Targets: TARGET_* (< 100) or POINT_* (>= 100). */
    /* ai:IsInsideTargetRegion(target, msbRegionId) [false] */
    int (*in_region)(AiAgent *a, int target, int region_id);
    /* ai:IsInsideObserve(slot): is `target` inside a sphere of `radius` m around dummy poly `dmy` of the agent;
       slot data come from ai:AddObserveChrDmySphere(slot, target, dmy, radius) [false] */
    int (*dmy_sphere)(AiAgent *a, int dmy, float radius, int target);
    /* ai:GetDistYSigned(target): target.y - self.y, metres; GetDistY = |.| [0] */
    float (*get_dist_y)(AiAgent *a, int target);
    /* ai:SetEventMoveTarget(msbPointId): the world resolves POINT_EVENT (102) to it afterwards [ignored] */
    void (*set_event_target)(AiAgent *a, int point_id);
} AiWorld;

struct AiAgent {
    const AiWorld *world;
    void *user;
    int obj_ref;                /* Lua table passed as `ai` */
    int logic_ref, logic_int_ref; /* Lua logic function (common10000_Logic) and its interrupt */
    AiGoal *root;               /* GOAL_COMMON_TopGoal */
    double excel[AI_MAX_EXCEL]; /* GetExcelParam(n) */
    double number[AI_MAX_NUMBERS];
    float timer[AI_MAX_TIMERS];
    uint32_t rng;               /* explicit seed: replays/traces must be deterministic */
    uint32_t rng_calls;
    int interrupt;              /* INTERUPT_* being dispatched, -1 none */
    int battle_state;           /* 1 = battle (target found), 0 = find */
    int prev_battle_state;
    int event_request;
    float ez_left;              /* seconds left in the current EzState action */
    int ez_state;
    float now, dt;
    int think_count;
    int move_target, turn_target, event_move_target;
    struct { int active, target, dmy; float radius; } observe[AI_MAX_OBSERVE];
};

typedef struct AiHost AiHost;

typedef struct {
    uint64_t lua_us;      /* time inside outermost Lua calls (needs a clock) */
    uint32_t lua_us_max;  /* worst single outermost call */
    uint32_t lua_calls;   /* all Lua calls (Activate/Update/Terminate/Interupt/logic) */
    int goals_peak;       /* goal pool high-water mark */
    int lua_kb;           /* Lua heap in use, KB (lua_getgccount) */
} AiStats;

AiHost *ai_host_new(void);
void ai_host_free(AiHost *h);
/* load one bytecode (or source) chunk and run it (registers goals, functions, constants) */
int ai_host_load(AiHost *h, const char *path);
void ai_host_set_clock(AiHost *h, uint64_t (*clock_us)(void)); /* optional: enables AiStats timing */
const AiStats *ai_host_stats(AiHost *h);
/* number of unknown ai:/goal: methods called so far, and their names (debug) */
int ai_host_unknown(AiHost *h, const char ***names, const int **counts);

int ai_agent_init(AiHost *h, AiAgent *a, const AiWorld *w, const char *logic_fn, const char *logic_int_fn,
                  uint32_t seed);
void ai_agent_free(AiHost *h, AiAgent *a); /* drops the goal tree (no Terminate calls) and the Lua objects */
/* one simulation step: think when the top goal stack is empty, then update the goal tree */
void ai_agent_tick(AiHost *h, AiAgent *a, float dt);
/* raise an INTERUPT_* event (Damaged, MissSwing, ...): dispatched down the active goal chain */
int ai_agent_interrupt(AiHost *h, AiAgent *a, int interupt);
const char *ai_goal_name(AiHost *h, int id);

#endif
