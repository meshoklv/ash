/*
 * PC unit tests of src/combat on the REAL cooked numbers (~/ash-assets/cooked/combat.ashcb, never in git).
 *
 *   tests/combat/run.sh            (builds with ~/opt/buildtools gcc, runs, exit code = failures)
 *
 * Actors are in bind pose (no skeleton is animated here): this checks rules, timing windows and numbers, not
 * how well the swing of a real animation lines up with the target - that needs the game and a screenshot.
 * Expected numbers are recomputed here from the data rows (not hard-coded FromSoftware values).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/combat/combat.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static CbData D;
static const float DT = 1.0f / 30.0f;

/* the "game": plays req_anims, advances clips; returns events of the step through the world */
static void game_step(CbWorld *w)
{
    int i;
    for (i = 0; i < w->n_actor; i++) {
        CbActor *a = w->actor[i];
        if (a->req_anim) {
            cb_begin_anim(w, a, a->req_anim);
            a->anim_id = a->req_anim; a->anim_t = 0; a->anim_t_prev = 0; a->req_anim = 0;
        } else if (a->hitstop <= 0) a->anim_t += DT;
    }
    combat_update(w, DT);
}

static int count_ev(const CbWorld *w, int type) { int i, n = 0; for (i = 0; i < w->n_ev; i++) n += w->ev[i].type == type; return n; }
static const CbEvent *find_ev(const CbWorld *w, int type) { int i; for (i = 0; i < w->n_ev; i++) if (w->ev[i].type == type) return &w->ev[i]; return 0; }

static void play(CbWorld *w, CbActor *a, uint32_t anim) { a->anim_id = anim; a->anim_t = 0; a->anim_t_prev = 0; a->win_anim = anim; }

static const CbHit *first_hit(const CbAnim *an, int kind)
{
    int i;
    for (i = 0; i < an->n_hit; i++) if (D.hit[an->hit_first + i].kind == kind) return &D.hit[an->hit_first + i];
    return 0;
}

/* place victim v so that attacker a's dummy (id, src) lies on v's torso capsule */
static void place_at_dummy(CbWorld *w, CbActor *a, int id, int src, CbActor *v)
{
    float p[3];
    if (!cb_dummy_world(w, a, id, src, p)) { printf("no dummy %d/%d\n", id, src); return; }
    /* dummy at the victim's chest height (1.2 m above its feet) */
    v->pos[0] = p[0]; v->pos[1] = p[1] - 1.2f; v->pos[2] = p[2];
}

static void face(CbActor *a, const CbActor *to, int away)
{
    float dx = to->pos[0] - a->pos[0], dz = to->pos[2] - a->pos[2];
    a->yaw = atan2f(-dx, -dz) + (away ? 3.14159265f : 0.0f);
}

static void *load_file(const char *path, int *size)
{
    FILE *f = fopen(path, "rb");
    void *buf;
    long n;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return 0; }
    fclose(f);
    *size = (int)n;
    return buf;
}

static void reset(CbWorld *w, CbActor *pl, CbActor *en, uint32_t npc, const char *kind)
{
    cb_world_init(w, &D);
    cb_actor_init_player(pl, &D);
    cb_actor_init_npc(en, &D, npc, kind);
    cb_world_add(w, pl);
    cb_world_add(w, en);
    play(w, pl, 0);
    play(w, en, 0);
}

int main(int argc, char **argv)
{
    char path[512];
    int size, rc, i;
    void *buf;
    CbWorld w;
    CbActor pl, en;
    const char *home = getenv("HOME");
    if (argc > 1) snprintf(path, sizeof path, "%s", argv[1]);
    else snprintf(path, sizeof path, "%s/ash-assets/cooked/combat.ashcb", home ? home : ".");
    buf = load_file(path, &size);
    if (!buf) { printf("cannot read %s (run tools/cook_combat.py)\n", path); return 2; }
    rc = cb_data_load(&D, buf, size);
    CHECK(rc == 0, "cb_data_load = %d", rc);
    if (rc) return 1;
    printf("data: %d kinds, %d anims, %d hits, %d atk, %d npc, %d throws\n", D.n_kind, D.n_anim, D.n_hit, D.n_atk,
           D.n_npc, D.n_throw);

    /* ------------------------------------------------ 1. damage: Longsword R1 vs Asylum hollow */
    {
        const CbAnim *r1 = cb_anim_find(&D, cb_kind_find(&D, "c0000"), 233000);
        const CbHit *h = r1 ? first_hit(r1, CB_HK_ATTACK) : 0;
        const CbAtk *atk = h ? &D.atk[h->atk] : 0;
        float exp_atk, exp_dmg, dmg, poise, stam;
        CHECK(r1 && h && atk, "a23_3000 has an attack window");
        reset(&w, &pl, &en, 250022, "c2500");   /* チュート 不死人 (Asylum hollow, 69 hp, phys def 77) */
        exp_atk = pl.wep_r->ar[0] * atk->pct_phys / 100.0f;
        exp_dmg = exp_atk - 0.79f * en.def[0] * expf(-0.27f * en.def[0] / exp_atk);
        dmg = cb_attack_damage(&w, &pl, atk, &en, &poise, &stam);
        printf("1. Longsword AR %.2f (base %u), R1 window frames %u-%u, %.0f%% phys -> hollow (def %.0f, hp %.0f): "
               "%.2f dmg, poise %.1f, %d hits to kill\n", pl.wep_r->ar[0], pl.wep_r->base_phys, h->start, h->end,
               (float)atk->pct_phys, en.def[0], en.hp, dmg, poise, (int)ceilf(en.hp / dmg));
        CHECK(fabsf(dmg - exp_dmg) < 0.01f, "R1 damage %.3f != %.3f", dmg, exp_dmg);
        CHECK(h->start == 16 && h->end == 20, "R1 window %u-%u", h->start, h->end);
        /* low-attack branch is continuous at atk == def */
        CHECK(fabsf(cb_damage_formula(100, 100) - cb_damage_formula(100.001f, 100)) < 0.5f, "formula continuity");

        /* the full pipeline: hollow on the sword tip, player swings */
        en.yaw = 0;
        place_at_dummy(&w, &pl, 100, 1, &en);
        rc = cb_begin_anim(&w, &pl, 233000);
        play(&w, &pl, 233000);
        {
            int hits = 0, hit_frame = -1, stagger = 0;
            float hp0 = en.hp;
            for (i = 0; i < 40; i++) {
                game_step(&w);
                if (count_ev(&w, CB_EV_HIT)) { hits += count_ev(&w, CB_EV_HIT); if (hit_frame < 0) hit_frame = i + 1; }
                stagger += count_ev(&w, CB_EV_STAGGER);
            }
            printf("   pipeline: %d hit(s) at frame %d, hp %.2f -> %.2f, stagger events %d, hollow anim %u\n", hits,
                   hit_frame, hp0, en.hp, stagger, en.anim_id);
            CHECK(hits == 1, "one hit per window, got %d", hits);
            CHECK(hit_frame >= 16 && hit_frame <= 20, "hit inside TAE window, frame %d", hit_frame);
            CHECK(fabsf((hp0 - en.hp) - exp_dmg) < 0.01f, "hp loss %.3f", hp0 - en.hp);
            CHECK(stagger == 1 && en.anim_id == D.kind[en.kind].react[CB_R_HIT_S], "hollow (poise 0) staggers: %u", en.anim_id);
        }
    }

    /* ------------------------------------------------ 2. i-frames of the roll (a00_0710) */
    {
        const CbAnim *roll = cb_anim_find(&D, 0, 710);
        int n = 0, first = -1, last = -1;
        for (i = 0; i < 40; i++) {
            if (cb_anim_flags(&D, roll, i / 30.0f) & CB_ST_INVINCIBLE) { n++; if (first < 0) first = i; last = i; }
        }
        printf("2. roll a00_0710: invincible frames %d (%d..%d) at 30 Hz\n", n, first, last);
        CHECK(n == 11 && first == 0 && last == 10, "roll i-frames %d (%d..%d), TAE says 8@0-11", n, first, last);
        /* a hollow swing that lands inside the i-frames does nothing; outside it hurts */
        {
            const CbAnim *ha = cb_anim_find(&D, cb_kind_find(&D, "c2500"), 3000);
            const CbHit *h = first_hit(ha, CB_HK_ATTACK);
            int roll_start, inv_ev = 0, hit_ev = 0, k;
            float hp0;
            for (k = 0; k < 2; k++) {
                reset(&w, &pl, &en, 250022, "c2500");
                place_at_dummy(&w, &en, D.atk[h->atk].dmy_a[0], 0, &pl);
                /* k=0: roll so that the hit window start lands on roll frame 2 (invincible); k=1: roll ends before */
                roll_start = k == 0 ? h->start - 2 : 0;
                hp0 = pl.hp;
                play(&w, &en, 3000);
                inv_ev = hit_ev = 0;
                for (i = 0; i < 60; i++) {
                    if (i == roll_start) { cb_begin_anim(&w, &pl, 710); play(&w, &pl, 710); }
                    game_step(&w);
                    inv_ev += count_ev(&w, CB_EV_INVULN);
                    hit_ev += count_ev(&w, CB_EV_HIT) + count_ev(&w, CB_EV_STAGGER) * 0;
                }
                printf("   hollow a3000 (window %u-%u) vs roll started at frame %d: invuln events %d, hits %d, hp %.0f -> %.0f\n",
                       h->start, h->end, roll_start, inv_ev, hit_ev, hp0, pl.hp);
                if (k == 0) CHECK(hit_ev == 0 && inv_ev >= 1 && pl.hp == hp0, "i-frames ignore the hit");
                else CHECK(hit_ev == 1 && pl.hp < hp0, "after the roll the hit lands");
            }
        }
    }

    /* ------------------------------------------------ 3. stamina */
    {
        float sp0, after;
        int n_ok = 0;
        reset(&w, &pl, &en, 250022, "c2500");
        en.pos[0] = 100;   /* out of reach */
        sp0 = pl.sp;
        rc = cb_begin_anim(&w, &pl, 233000); play(&w, &pl, 233000);
        after = pl.sp;
        for (i = 0; i < 20; i++) game_step(&w);
        printf("3. stamina %.0f, R1 costs %u -> %.0f; at frame 20 (TAE 225 regen 0%% window) %.1f\n", sp0,
               cb_anim_find(&D, 0, 233000)->stamina, after, pl.sp);
        CHECK(rc == 1 && fabsf(after - (sp0 - 20)) < 0.01f, "R1 costs 20 (BehaviorParam_PC 102300000)");
        CHECK(fabsf(pl.sp - after) < 0.01f, "no regen inside the attack's 225 window");
        play(&w, &pl, 0);
        for (i = 0; i < 30; i++) game_step(&w);
        printf("   1 s idle later: %.1f (regen %.0f/s)\n", pl.sp, pl.sp_regen);
        CHECK(fabsf(pl.sp - sp0) < 0.01f, "refilled after 1 s idle");
        /* spam: an action needs stamina > 0 */
        for (i = 0; i < 10; i++) n_ok += cb_begin_anim(&w, &pl, 233000);
        printf("   R1 spam from full: %d allowed, then refused (stamina %.1f)\n", n_ok, pl.sp);
        CHECK(n_ok == (int)ceilf(sp0 / 20.0f), "%d attacks allowed", n_ok);
        /* guard: regen x0.2 [версия] */
        pl.sp = 10; pl.guard_input = 1; play(&w, &pl, 1);
        for (i = 0; i < 30; i++) game_step(&w);
        printf("   1 s in the guard anim a00_0001 (TAE 225 = 20%%) from 10: %.1f\n", pl.sp);
        CHECK(fabsf(pl.sp - (10 + pl.sp_regen * 0.2f)) < 0.1f, "guard regen 20%% from the TAE");
    }

    /* ------------------------------------------------ 4. block with the Knight Shield */
    {
        const CbAnim *ha = cb_anim_find(&D, cb_kind_find(&D, "c2500"), 3000);
        const CbHit *h = first_hit(ha, CB_HK_ATTACK);
        const CbEvent *e;
        float hp0, sp0, stam;
        int blocks = 0;
        reset(&w, &pl, &en, 250022, "c2500");
        place_at_dummy(&w, &en, D.atk[h->atk].dmy_a[0], 0, &pl);
        face(&pl, &en, 0);               /* face the hollow */
        pl.guard_input = 1;
        play(&w, &pl, 1);
        hp0 = pl.hp; sp0 = pl.sp;
        cb_attack_damage(&w, &en, &D.atk[h->atk], &pl, 0, &stam);
        play(&w, &en, 3000);
        e = 0;
        static CbEvent blk;
        for (i = 0; i < 45; i++) { game_step(&w); if (find_ev(&w, CB_EV_BLOCK)) { blk = *find_ev(&w, CB_EV_BLOCK); e = &blk; blocks++;
            printf("4. block: hp %.1f -> %.1f (shield phys cut %.0f%%), stamina %.1f -> %.1f (atk stamina %.0f, stability %u), react anim %u\n",
                   hp0, pl.hp, pl.wep_l->guard_cut[0], sp0, pl.sp, stam, pl.wep_l->stability, e->anim); } }
        CHECK(blocks == 1, "blocked once");
        CHECK(fabsf(pl.hp - hp0) < 0.01f, "100%% physical shield takes no hp");
        CHECK(e && fabsf(e->stamina - stam * (1 - pl.wep_l->stability / 100.0f)) < 0.01f, "stamina loss");
        /* from behind the shield does nothing */
        reset(&w, &pl, &en, 250022, "c2500");
        place_at_dummy(&w, &en, D.atk[h->atk].dmy_a[0], 0, &pl);
        face(&pl, &en, 1); pl.guard_input = 1; play(&w, &pl, 1);
        hp0 = pl.hp;
        play(&w, &en, 3000);
        blocks = 0;
        for (i = 0; i < 45; i++) { game_step(&w); blocks += count_ev(&w, CB_EV_BLOCK); }
        printf("   hit from behind while guarding: blocks %d, hp %.1f -> %.1f\n", blocks, hp0, pl.hp);
        CHECK(blocks == 0 && pl.hp < hp0, "no block from behind");
    }

    /* ------------------------------------------------ 5. parry + riposte, backstab, death */
    {
        const CbAnim *ha = cb_anim_find(&D, cb_kind_find(&D, "c2500"), 3000);
        const CbAnim *pa = cb_anim_find(&D, 0, 485100);
        const CbHit *ph = first_hit(pa, CB_HK_ATTACK);
        int parry_frame = -1, start, v, throw_hit = -1, deaths = 0, t;
        float hp0, pp[3], dmg = 0;
        uint32_t ripo = 0;
        int par = 0;
        for (t = 0; t < ha->n_state; t++) {
            const CbState *s = &D.state[ha->state_first + t];
            if (s->raw == 5) { par = s->start; break; }
        }
        reset(&w, &pl, &en, 250022, "c2500");
        /* put the hollow on the parry hitbox (dummy 131 of the player), facing the player */
        cb_dummy_world(&w, &pl, 131, 0, pp);
        printf("   player dummy 131 (parry) at %.2f %.2f %.2f\n", pp[0], pp[1], pp[2]);
        place_at_dummy(&w, &pl, 131, 0, &en);
        face(&en, &pl, 0);
        play(&w, &en, 3000);
        start = par - 3;    /* parry window 0-7 covers the hollow's parry-possible frame */
        for (i = 0; i < 40; i++) {
            if (i == start) { cb_begin_anim(&w, &pl, 485100); play(&w, &pl, 485100); }
            game_step(&w);
            if (find_ev(&w, CB_EV_PARRY) && parry_frame < 0) parry_frame = i + 1;
        }
        printf("5. parry: hollow a3000 parry-possible (TAE flag 5) from frame %d, player parry a48_5100 window %u-%u "
               "started at %d -> parry at frame %d, hollow anim %u, ripostable %d\n", par, ph->start, ph->end, start,
               parry_frame, en.anim_id, !!(en.flags & CB_ST_RIPOSTABLE));
        CHECK(parry_frame > 0 && en.anim_id == 2100, "parried into a2100");
        CHECK(pl.hp == pl.hp_max, "parried attack did no damage");
        /* riposte */
        en.pos[0] = pl.pos[0]; en.pos[2] = pl.pos[2] - 1.0f; en.yaw = pl.yaw + 3.14159265f;
        hp0 = en.hp;
        v = cb_try_throw(&w, 0);
        ripo = pl.req_anim;
        for (i = 0; i < 60; i++) {
            game_step(&w);
            if (i == 0) CHECK(count_ev(&w, CB_EV_THROW) == 1, "THROW event reaches the next update's list");
            if (find_ev(&w, CB_EV_THROW_HIT) && throw_hit < 0) { throw_hit = i; dmg = find_ev(&w, CB_EV_THROW_HIT)->damage; }
            deaths += count_ev(&w, CB_EV_DEATH);
        }
        printf("   riposte: victim %d, player anim %u, hollow anim %u, damage %.1f at step %d, hp %.1f -> %.1f, deaths %d\n",
               v, ripo, en.anim_id, dmg, throw_hit, hp0, en.hp, deaths);
        CHECK(v == 1 && ripo == 233201, "riposte = ThrowParam 22500 attacker a3201 in the weapon's category");
        CHECK(en.anim_id == 2510, "victim anim 2510");
        CHECK(throw_hit >= 24 && throw_hit <= 26, "riposte damage at the e304 window (frame 25)");
        CHECK(dmg > 2.0f * 39.0f, "riposte (250%%) hits harder than 2 R1");
        CHECK(deaths == 1 && en.dead, "the hollow dies from the riposte");
        /* backstab */
        reset(&w, &pl, &en, 250025, "c2500");
        en.pos[2] = -1.0f; en.yaw = pl.yaw;     /* in front of the player, back turned */
        v = cb_try_throw(&w, 0);
        printf("   backstab: victim %d, player anim %u, hollow anim %u\n", v, pl.req_anim, en.req_anim);
        CHECK(v == 1 && pl.req_anim == 233400 && en.req_anim == 2500, "backstab = ThrowParam 12500");
        reset(&w, &pl, &en, 250025, "c2500");
        en.pos[2] = -1.0f; en.yaw = pl.yaw + 3.14159265f;  /* facing the player: no backstab, no riposte */
        CHECK(cb_try_throw(&w, 0) < 0, "no throw from the front");
    }

    /* ------------------------------------------------ 6. Asylum Demon (c2232, NpcParam 223200) vs the player */
    {
        int kd = cb_kind_find(&D, "c2232");
        const CbAnim *da = cb_anim_find(&D, kd, 3000);
        const CbHit *h = first_hit(da, CB_HK_ATTACK);
        float hp0, dmg_exp;
        int hits = 0;
        reset(&w, &pl, &en, 223200, "c2232");
        place_at_dummy(&w, &en, D.atk[h->atk].dmy_a[0], 0, &pl);
        hp0 = pl.hp;
        dmg_exp = cb_attack_damage(&w, &en, &D.atk[h->atk], &pl, 0, 0);
        play(&w, &en, 3000);
        for (i = 0; i < 60; i++) { game_step(&w); hits += count_ev(&w, CB_EV_HIT); }
        printf("6. Asylum Demon a3000 (window %u-%u, phys %u, dmgLevel %u) vs player (def %.0f, poise %.0f): hits %d, hp %.0f -> %.1f "
               "(exp %.1f), player anim %u\n", h->start, h->end, D.atk[h->atk].atk_phys, D.atk[h->atk].dmg_level,
               pl.def[0], pl.poise_max, hits, hp0, pl.hp, dmg_exp, pl.anim_id);
        CHECK(hits >= 1 && fabsf(hp0 - pl.hp - dmg_exp) < 0.01f, "demon hit");
        CHECK(pl.anim_id == D.kind[0].react[CB_R_KNOCKDOWN], "demon knocks the player down");
        /* the demon never staggers (no hit reactions in its TAE) */
        {
            const CbAnim *r1 = cb_anim_find(&D, 0, 233000);
            const CbHit *ph = first_hit(r1, CB_HK_ATTACK);
            float d1;
            reset(&w, &pl, &en, 223200, "c2232");
            d1 = cb_attack_damage(&w, &pl, &D.atk[ph->atk], &en, 0, 0);
            printf("   R1 on the demon (def %.0f, hp %.0f): %.2f dmg -> %d hits\n", en.def[0], en.hp, d1, (int)ceilf(en.hp / d1));
            CHECK(D.kind[kd].react[CB_R_HIT_S] == 0, "demon has no stagger anim");
        }
    }

    /* ------------------------------------------------ 7. death of the player -> event + anim */
    {
        const CbNpc *sd = cb_npc_find(&D, 223000);
        printf("   Stray Demon (c2230, NpcParam 223000) with its level SpEffect %u: hp %u, phys def %d, attack x%.2f\n",
               sd->level_speffect, sd->hp, sd->def_phys, sd->atk_mult);
        CHECK(sd->hp == 5250, "2100 x 2.5 (SpEffect 7014)");
        reset(&w, &pl, &en, 223200, "c2232");
        pl.hp = 1;
        {
            const CbAnim *da = cb_anim_find(&D, cb_kind_find(&D, "c2232"), 3000);
            const CbHit *h = first_hit(da, CB_HK_ATTACK);
            int deaths = 0;
            place_at_dummy(&w, &en, D.atk[h->atk].dmy_a[0], 0, &pl);
            play(&w, &en, 3000);
            for (i = 0; i < 60; i++) { game_step(&w); deaths += count_ev(&w, CB_EV_DEATH); }
            printf("7. player death: events %d, anim %u, dead %d\n", deaths, pl.anim_id, pl.dead);
            CHECK(deaths == 1 && pl.dead && pl.anim_id == 6000, "death a00_6000");
            CHECK(cb_begin_anim(&w, &pl, 233000) == 0, "dead cannot act");
        }
    }

    /* ------------------------------------------------ 8. repel: hollow swings bounce off the Knight Shield, the demon's do not */
    {
        int k;
        for (k = 0; k < 2; k++) {
            const char *kind = k ? "c2232" : "c2500";
            int kk = cb_kind_find(&D, kind), j;
            const CbAnim *ha = cb_anim_find(&D, kk, 3000);
            const CbHit *h = first_hit(ha, CB_HK_ATTACK);
            /* the demon's a3000 smash is IgnoreGuard in AtkParam_Npc 223200: take its first blockable attack */
            for (j = 0; k && j < D.kind[kk].n_anim; j++) {
                const CbAnim *an = &D.anim[D.kind[kk].anim_first + j];
                const CbHit *hh = an->anim_id >= 3000 ? first_hit(an, CB_HK_ATTACK) : 0;
                if (hh && hh->atk < D.n_atk && !(D.atk[hh->atk].flags & CB_ATK_IGNORE_GUARD)) { ha = an; h = hh; break; }
            }
            int blocks = 0, repels = 0;
            uint32_t rep_anim = 0;
            reset(&w, &pl, &en, k ? 223200 : 250022, kind);
            place_at_dummy(&w, &en, D.atk[h->atk].dmy_a[0], 0, &pl);
            face(&pl, &en, 0); pl.guard_input = 1; play(&w, &pl, 1);
            play(&w, &en, ha->anim_id);
            for (i = 0; i < 90; i++) {
                game_step(&w);
                blocks += count_ev(&w, CB_EV_BLOCK) + count_ev(&w, CB_EV_GUARD_BREAK);
                if (count_ev(&w, CB_EV_HIT)) printf("   (unblocked hit, player at %.2f %.2f, yaw %.2f)\n", pl.pos[0], pl.pos[2], pl.yaw);
                if (find_ev(&w, CB_EV_REPEL)) { repels++; rep_anim = find_ev(&w, CB_EV_REPEL)->anim; }
            }
            printf("8. %s a%u (GuardAttackPower %u) on the Knight Shield (DeflectOnBlock %u): blocks %d, repels %d, "
                   "attacker anim %u\n", kind, ha->anim_id, D.atk[h->atk].guard_atk_rate, pl.wep_l->deflect_block, blocks, repels, rep_anim);
            if (!k) CHECK(blocks == 1 && repels == 1 && rep_anim == 2050 && en.anim_id == 2050, "hollow bounces into a2050");
            else CHECK(blocks == 1 && repels == 0, "the demon is not repelled");
        }
    }

    /* ------------------------------------------------ 9. plunge: 3800 -> 3801 -> 3810, one hit, AttackType 62 */
    {
        uint32_t ids[3];
        int hits = 0, t62 = 0, step, kd;
        float hp0, dmg = 0, exp_d, sp0, sp1;
        const CbHit *fh;
        ids[0] = cb_attack_anim(&w, &pl, CB_ACT_PLUNGE, 0);
        ids[1] = cb_attack_anim(&w, &pl, CB_ACT_PLUNGE_FALL, 0);
        ids[2] = cb_attack_anim(&w, &pl, CB_ACT_PLUNGE_LAND, 0);
        CHECK(ids[0] == 233800 && ids[1] == 233801 && ids[2] == 233810, "plunge anims %u %u %u", ids[0], ids[1], ids[2]);
        for (kd = 0; kd < 2; kd++) {
            reset(&w, &pl, &en, kd ? 223200 : 250022, kd ? "c2232" : "c2500");
            fh = first_hit(cb_anim_find(&D, 0, 233800), CB_HK_ATTACK);
            exp_d = cb_attack_damage(&w, &pl, &D.atk[fh->atk], &en, 0, 0);
            /* the victim right under the falling sword point (dummy 130) */
            place_at_dummy(&w, &pl, 130, 1, &en);
            hp0 = en.hp; hits = t62 = 0;
            for (step = 0; step < 3; step++) {
                sp0 = pl.sp;
                CHECK(cb_begin_anim(&w, &pl, ids[step]) == 1, "plunge step %d allowed", step);
                sp1 = pl.sp;
                play(&w, &pl, ids[step]);
                for (i = 0; i < 40; i++) {
                    game_step(&w);
                    if (find_ev(&w, CB_EV_HIT)) { hits++; dmg = find_ev(&w, CB_EV_HIT)->damage; t62 += find_ev(&w, CB_EV_HIT)->atk_type == 62; }
                    if (en.req_anim) en.req_anim = 0;
                    if (pl.req_anim) pl.req_anim = 0;
                }
                if (step == 2) printf("   landing a23_3810 stamina %.0f -> %.0f\n", sp0, sp1);
            }
            printf("9. plunge on %s (def %.0f): hits %d (AttackType 62: %d), damage %.1f (AtkParam %u: %u%% AR, dmgLevel %u), "
                   "hp %.0f -> %.0f\n", kd ? "the Asylum Demon" : "a hollow", en.def[0], hits, t62, dmg, D.atk[fh->atk].id,
                   D.atk[fh->atk].pct_phys, D.atk[fh->atk].dmg_level, hp0, en.hp);
            CHECK(hits == 1 && t62 == 1, "one plunge hit over 3800/3801/3810");
            CHECK(fabsf(dmg - exp_d) < 0.01f || en.dead, "plunge damage %.2f exp %.2f", dmg, exp_d);
            play(&w, &pl, 0); game_step(&w);
            CHECK(pl.plunge_hit == 0, "plunge memory cleared after the plunge");
        }
    }

    /* ------------------------------------------------ 10. running attack, heavy, rolling attack, two-handed ids */
    {
        struct { int act; const char *name; } T[3] = {{CB_ACT_RUN_R1, "running R1"}, {CB_ACT_R2, "heavy R2"}, {CB_ACT_ROLL_R1, "rolling R1"}};
        int k;
        CHECK(cb_attack_anim(&w, &pl, CB_ACT_R1, 1) == 234000 && cb_attack_anim(&w, &pl, CB_ACT_R2, 1) == 234300,
              "two-handed = +1000");
        CHECK(cb_attack_anim(&w, &pl, 3777, 0) == 0, "unknown action -> 0");
        for (k = 0; k < 3; k++) {
            uint32_t id;
            const CbAnim *an;
            const CbHit *h;
            float sp0, hp0, exp_d;
            int hits = 0, stag = 0;
            uint32_t react_anim = 0;
            reset(&w, &pl, &en, 250022, "c2500");
            id = cb_attack_anim(&w, &pl, T[k].act, 0);
            an = cb_anim_find(&D, 0, id);
            h = an ? first_hit(an, CB_HK_ATTACK) : 0;
            CHECK(id && h, "%s anim", T[k].name);
            if (!h) continue;
            exp_d = cb_attack_damage(&w, &pl, &D.atk[h->atk], &en, 0, 0);
            place_at_dummy(&w, &pl, D.atk[h->atk].dmy_a[0], 1, &en);
            sp0 = pl.sp; hp0 = en.hp;
            cb_begin_anim(&w, &pl, id); play(&w, &pl, id);
            CHECK(fabsf(sp0 - pl.sp - an->stamina) < 0.01f, "%s stamina", T[k].name);
            for (i = 0; i < 45; i++) {
                game_step(&w);
                hits += count_ev(&w, CB_EV_HIT);
                if (find_ev(&w, CB_EV_STAGGER)) { stag++; react_anim = find_ev(&w, CB_EV_STAGGER)->anim; }
            }
            printf("10. %s a23_%u: window %u-%u, stamina %u, AtkParam %u %u%%, dmgLevel %u -> hits %d, %.1f dmg (exp %.1f), "
                   "hollow stagger %d anim %u\n", T[k].name, id % 10000, h->start, h->end, an->stamina, D.atk[h->atk].id,
                   D.atk[h->atk].pct_phys, D.atk[h->atk].dmg_level, hits, hp0 - en.hp, exp_d, stag, react_anim);
            CHECK(hits == 1 && fabsf(hp0 - en.hp - exp_d) < 0.01f, "%s hits once", T[k].name);
            CHECK(stag == 1 && react_anim == D.kind[en.kind].react[D.atk[h->atk].dmg_level <= 1 ? CB_R_HIT_S : CB_R_HIT_M],
                  "%s stagger by dmgLevel", T[k].name);
        }
    }

    /* ------------------------------------------------ 11. player poise: hollow hits until the Elite set's 46 breaks */
    {
        const CbAnim *ha = cb_anim_find(&D, cb_kind_find(&D, "c2500"), 3000);
        const CbHit *h = first_hit(ha, CB_HK_ATTACK);
        int n, hits = 0, first_stag = -1;
        float pd = 0;
        reset(&w, &pl, &en, 250022, "c2500");
        cb_attack_damage(&w, &en, &D.atk[h->atk], &pl, &pd, 0);
        for (n = 0; n < 7 && first_stag < 0; n++) {
            place_at_dummy(&w, &en, D.atk[h->atk].dmy_a[0], 0, &pl);
            cb_begin_anim(&w, &en, 3000); play(&w, &en, 3000);
            for (i = 0; i < 40; i++) {
                game_step(&w);
                hits += count_ev(&w, CB_EV_HIT);
                if (find_ev(&w, CB_EV_STAGGER) && first_stag < 0) first_stag = hits;
            }
            play(&w, &pl, 0);
        }
        printf("11. player poise %.0f, hollow poise damage %.0f per hit: first stagger on hit %d (anim %u), hp %.0f\n",
               pl.poise_max, pd, first_stag, pl.anim_id, pl.hp);
        CHECK(first_stag == (int)ceilf(pl.poise_max / pd), "poise breaks on hit %d", first_stag);
    }

    printf("%d/%d checks passed\n", checks - fails, checks);
    free(buf);
    return fails;
}
