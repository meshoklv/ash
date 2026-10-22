#include "camera.h"

#include <math.h>

#define DEG (3.14159265f / 180.0f)

CamCfg cam_default_cfg(void)
{
    CamCfg c;
    c.dist = 4.0f;            /* LockCamParam 0 camDistTarget [DSR] */
    c.pivot_y = 1.42f;        /* chrOrgOffset_Y [DSR] */
    c.fov_deg = 43.0f;        /* camFovY [DSR] */
    c.pitch_min_deg = -40.0f; /* rotRangeMinX [DSR] */
    c.pitch_max_deg = 50.0f;
    c.pitch_default_deg = 15.0f;
    c.omega_h = 10.0f;
    c.omega_v = 6.0f;
    c.pivot_lag_h = 0.5f;          /* [придумал] */
    c.pivot_lag_v = 0.15f;
    c.autoyaw_delay = 0.6f;
    c.autoyaw_rate_max_deg = 60.0f;
    c.autoyaw_deadband_deg = 10.0f;
    c.autoyaw_toward_cam_deg = 120.0f;
    c.reset_time = 0.25f;
    c.claw_yaw_rate_deg = 120.0f;
    c.claw_pitch_rate_deg = 60.0f;
    c.claw_ramp = 0.15f;
    c.pitch_return_delay = 1.5f;
    c.pitch_return_rate_deg = 20.0f;
    c.col_radius = 0.2f;
    c.col_min_dist = 0.8f;
    c.col_out_speed = 3.0f;
    c.col_escape_dist = 2.0f;      /* [придумал] */
    c.col_escape_step_deg = 20.0f;
    c.col_escape_max_deg = 60.0f;
    c.col_escape_rate_deg = 90.0f;
    c.col_side_step_deg = 30.0f;   /* [придумал, bug "route boss fight: camera loses the knight", 14.10] */
    c.col_side_max_deg = 120.0f;
    c.col_side_rate_deg = 270.0f;
    c.col_search_hold = 0.2f;      /* s [придумал, bug "camera cost", 15.10] */
    c.col_search_probes = 6;       /* probes a step at most */
    c.close_dist = 3.6f;
    c.close_lock_slide = 1.2f;     /* m, look-at toward the lock target at most */
    c.close_look_y = 0.85f;        /* body centre of the knight (~1.7 m tall), also low enough for a roll */
    c.lock_yaw_omega = 8.0f;
    c.lock_yaw_max_deg = 240.0f;
    c.lock_pitch_omega = 6.0f;
    c.lock_pitch_min_deg = -40.0f;
    c.lock_pitch_max_deg = 30.0f;
    c.lock_pitch_shift = 0.6f;
    c.row_blend_time = 0.5f;
    return c;
}

/* DSR GameParam LockCamParam (the controls design notes §2.1, read with Soulstruct) */
static const CamRow g_rows[] = {
    {0, 4.0f, 1.42f, -40.0f, 0.6f, 15.0f, 43.0f},
    {101, 2.2f, 1.22f, -40.0f, 0.6f, 15.0f, 43.0f},
    {102, 3.2f, 1.32f, -40.0f, 0.6f, 15.0f, 43.0f},
    {103, 5.0f, 1.60f, -20.0f, 0.4f, 15.0f, 43.0f},
    {1100, 6.0f, 1.80f, -30.0f, 0.4f, 15.0f, 48.0f},
};

const CamRow *cam_row(int id)
{
    for (unsigned i = 0; i < sizeof(g_rows) / sizeof(g_rows[0]); i++)
        if (g_rows[i].id == id) return &g_rows[i];
    return &g_rows[0];
}

static float wrap_pi(float a)
{
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

/* critically damped spring toward target (exact for constant target over dt) */
static void spring(float *x, float *v, float target, float omega, float dt)
{
    float d = *x - target;
    float e = expf(-omega * dt);
    float tmp = (*v + omega * d) * dt;
    *v = (*v - omega * tmp) * e;
    *x = target + (d + tmp) * e;
}

static void eye_dir(float yaw, float pitch, float dir[3])
{
    float cp = cosf(pitch);
    dir[0] = sinf(yaw) * cp; dir[1] = sinf(pitch); dir[2] = cosf(yaw) * cp;
}

static float cast_dir(const CamState *s, const CamCfg *c, const CamInput *in, const float dir[3], float want)
{
    float far[3] = {s->pivot[0] + dir[0] * want, s->pivot[1] + dir[1] * want, s->pivot[2] + dir[2] * want};
    return in->cast(in->cast_ctx, s->pivot, far, c->col_radius);
}

static void place_eye(CamState *s, const CamCfg *c, const CamInput *in, float dt)
{
    float want = s->row_dist + s->frame_extra;
    float pmax = 75.0f * DEG, dir[3];
    s->hit_frac = 1.0f;
    float target_dist = want;
    if (in && in->cast) {
        /* Where to put the eye when straight behind is blocked: climb over the wall (pitch +20/40/60), else swing
           aside along it (yaw 30..120 deg either side, pitch +0/+20) [придумал, 14.10]. That search is 33 probes;
           run in full every step it was up to 35 sphere casts a step while the knight stood at the arena wall, and
           the emulated frame rate halved there, 56 -> 29 fps (route run f21900-23700, bug "camera cost", 15.10).
           Now [придумал, 15.10]:
           - "is it free" probes cast only `need` (2 m), not the full distance (half the samples);
           - straight behind is probed every step (1 cast); while blocked, the last answer is kept col_search_hold
             s and only its direction is re-probed (1 cast);
           - a new search runs at most col_search_probes probes a step and goes on next step; meanwhile the old
             answer stays. A step costs at most 1 + col_search_probes + 1 casts. */
        float need = c->col_escape_dist < want ? c->col_escape_dist : want, want_cp = 0, want_cy = 0;
        eye_dir(s->yaw, s->pitch, dir);
        float base_f = cast_dir(s, c, in, dir, need);
        if (base_f >= 0.999f) {
            s->col_hold = s->col_tgt_cy = s->col_tgt_cp = 0;
            s->col_tgt_ok = 0;
            s->col_si = -1;
        } else {
            want_cy = s->col_tgt_cy;
            want_cp = s->col_tgt_cp;
            if (s->col_si < 0) {
                if (s->col_hold > 0) {
                    s->col_hold -= dt > 0 ? dt : 1.0f;
                    if (s->col_tgt_ok) {   /* the kept direction closed: search again */
                        float p = s->pitch + s->col_tgt_cp;
                        if (p > pmax) p = pmax;
                        eye_dir(s->yaw + s->col_tgt_cy, p, dir);
                        if (cast_dir(s, c, in, dir, need) < 0.999f) s->col_hold = 0;
                    }
                }
                if (s->col_hold <= 0) {   /* start a search; the swing it already has is tried first */
                    s->col_si = 0;
                    s->col_s_first = s->col_yaw;
                    s->col_s_lean = s->col_yaw < 0 ? -1.0f : 1.0f;
                    s->col_s_best = base_f * need;
                    s->col_s_cy = s->col_s_cp = 0;
                }
            }
            if (s->col_si >= 0) {
                /* probe list: climb +20/40/60, then each side candidate (current swing, lean*30, -lean*30,
                   lean*60, ...) at pitch +0 and +20 */
                int n_climb = (int)(c->col_escape_max_deg / c->col_escape_step_deg + 0.5f);
                int n_side = (int)(c->col_side_max_deg / c->col_side_step_deg + 0.5f) * 2;
                int has_first = fabsf(s->col_s_first) > 1e-3f;
                int total = n_climb + 2 * (n_side + has_first), found = 0;
                for (int k = 0; k < c->col_search_probes && s->col_si < total && !found; k++, s->col_si++) {
                    int i = s->col_si;
                    float cy = 0, p, margin;
                    if (i < n_climb) {
                        p = s->pitch + (i + 1) * c->col_escape_step_deg * DEG;
                        margin = 0.25f;
                    } else {
                        int j = (i - n_climb) / 2, e = (i - n_climb) % 2;
                        if (has_first && j == 0) cy = s->col_s_first;
                        else {
                            int o = j - has_first;   /* 0: lean*30, 1: -lean*30, 2: lean*60, ... */
                            cy = (o & 1 ? -s->col_s_lean : s->col_s_lean) * (float)(o / 2 + 1) * c->col_side_step_deg * DEG;
                        }
                        p = s->pitch + e * c->col_escape_step_deg * DEG;
                        margin = 0.5f;   /* a swing must be clearly freer than a climb */
                    }
                    if (p > pmax) p = pmax;
                    eye_dir(s->yaw + cy, p, dir);
                    float fd = cast_dir(s, c, in, dir, need) * need;
                    if (fd >= need * 0.999f) { s->col_s_cy = cy; s->col_s_cp = p - s->pitch; found = 1; }
                    else if (fd > s->col_s_best + margin) { s->col_s_best = fd; s->col_s_cy = cy; s->col_s_cp = p - s->pitch; }
                }
                if (found || s->col_si >= total) {
                    s->col_tgt_cy = want_cy = s->col_s_cy;
                    s->col_tgt_cp = want_cp = s->col_s_cp;
                    s->col_tgt_ok = found;
                    s->col_hold = c->col_search_hold;
                    s->col_si = -1;
                }
            }
        }
        if (want_cp < 0) want_cp = 0;
        float st = c->col_escape_rate_deg * DEG * (dt > 0 ? dt : 1.0f);
        float dcp = want_cp - s->col_pitch;
        s->col_pitch += dcp > st ? st : dcp < -st * 0.5f ? -st * 0.5f : dcp;   /* climb fast, come down slower */
        float sy = c->col_side_rate_deg * DEG * (dt > 0 ? dt : 1.0f), dcy = want_cy - s->col_yaw;
        if (want_cy == 0) sy *= 0.5f;                                            /* swing back slower */
        s->col_yaw += dcy > sy ? sy : dcy < -sy ? -sy : dcy;
        float p = s->pitch + s->col_pitch;
        if (p > pmax) p = pmax;
        eye_dir(s->yaw + s->col_yaw, p, dir);
        float f = cast_dir(s, c, in, dir, want);
        s->hit_frac = f;
        if (f < 1.0f) {
            target_dist = f * want - 0.05f;
            if (target_dist < c->col_min_dist) target_dist = c->col_min_dist;
        }
    } else {
        s->col_pitch = s->col_yaw = 0;
        eye_dir(s->yaw, s->pitch, dir);
    }
    if (dt <= 0 || target_dist < s->cur_dist) s->cur_dist = target_dist;        /* pull in at once */
    else {
        /* push out smoothly; from very close (< 2 m: knight hidden or cut off) 2.5x faster - a column or rubble
           brushed in a roll pulls the camera in for 2-3 frames, and at 3 m/s it then stayed on the rubble with no
           knight for ~0.5 s (route run f7810-7830) [придумал, 14.10] */
        float out = s->cur_dist < 2.0f ? c->col_out_speed * 2.5f : c->col_out_speed;
        s->cur_dist += out * dt;
        if (s->cur_dist > target_dist) s->cur_dist = target_dist;
    }
    for (int i = 0; i < 3; i++) s->eye[i] = s->pivot[i] + dir[i] * s->cur_dist;
    /* pulled in: the pivot is 1.3-1.7 m above the feet, at 0.8 m it leaves the body below the frame -> look lower */
    /* locked: also slide the look-at toward the target so a camera that climbed still shows the enemy */
    float drop = 0, slide[2] = {0, 0};
    if (in && s->cur_dist < c->close_dist) {
        float k = (c->close_dist - s->cur_dist) / (c->close_dist - c->col_min_dist);
        if (k > 1) k = 1;
        float h = s->pivot[1] - in->char_pos[1] - c->close_look_y;
        if (h > 0) drop = k * h;
        /* closer than 0.7 m the game hides the knight (asy_main.c): nothing to look down at, and the drop would point
           the camera at the floor; fade it out down to 0.35 m (the game's col_min_dist) [придумал, 14.10] */
        if (s->cur_dist < 0.7f) drop *= s->cur_dist > 0.35f ? (s->cur_dist - 0.35f) / 0.35f : 0.0f;
        if (in->locked) {
            float tx = in->target[0] - s->pivot[0], tz = in->target[2] - s->pivot[2], td = sqrtf(tx * tx + tz * tz);
            /* <= 0.4 * distance: a camera 0.8 m over the knight sliding 1.2 m ahead drops him under the frame
               (boss run f5732: pitch 12+60 deg, the rolled knight 22 deg below the centre) */
            float sl = td * 0.35f, cap = s->cur_dist * 0.4f;
            if (cap > c->close_lock_slide) cap = c->close_lock_slide;
            if (sl > cap) sl = cap;
            if (td > 0.1f) { slide[0] = k * sl * tx / td; slide[1] = k * sl * tz / td; }
        }
    }
    float kd = dt <= 0 ? 1.0f : dt * 8.0f > 1 ? 1.0f : dt * 8.0f;
    s->look_drop += (drop - s->look_drop) * kd;
    s->look_slide[0] += (slide[0] - s->look_slide[0]) * kd;
    s->look_slide[1] += (slide[1] - s->look_slide[1]) * kd;
    s->at[0] = s->pivot[0] + s->look_slide[0]; s->at[1] = s->pivot[1] - s->look_drop;
    s->at[2] = s->pivot[2] + s->look_slide[1];
}

static void blend_row(CamState *s, const CamCfg *c, const CamRow *r, float dt)
{
    float k = dt <= 0 ? 1.0f : dt / (c->row_blend_time * 0.33f);
    if (k > 1) k = 1;
    s->row_dist += (r->dist - s->row_dist) * k;
    s->row_pivot_y += (r->pivot_y - s->row_pivot_y) * k;
    s->row_fov += (r->fov_deg - s->row_fov) * k;
    s->row_pitch_min += (r->pitch_min_deg - s->row_pitch_min) * k;
    s->row_shift += (r->lock_shift - s->row_shift) * k;
}

/* spring toward an angle with a rate cap */
static void spring_angle(float *x, float *v, float target, float omega, float max_rate, float dt)
{
    float d = wrap_pi(*x - target);
    float e = expf(-omega * dt);
    float tmp = (*v + omega * d) * dt;
    float nv = (*v - omega * tmp) * e;
    float nx = target + (d + tmp) * e;
    float step = wrap_pi(nx - *x);
    if (max_rate > 0) {
        float lim = max_rate * dt;
        if (step > lim) step = lim;
        if (step < -lim) step = -lim;
        if (nv > max_rate) nv = max_rate;
        if (nv < -max_rate) nv = -max_rate;
    }
    *x = wrap_pi(*x + step);
    *v = nv;
}

void cam_reset(CamState *s, const CamCfg *c, const CamRow *row, const float pos[3], float yaw)
{
    s->row_dist = row->dist; s->row_pivot_y = row->pivot_y; s->row_fov = row->fov_deg;
    s->row_pitch_min = row->pitch_min_deg; s->row_shift = row->lock_shift;
    s->frame_extra = s->frame_lift = 0;
    s->col_hold = s->col_tgt_cy = s->col_tgt_cp = s->col_s_first = s->col_s_lean = s->col_s_best = 0;
    s->col_s_cy = s->col_s_cp = 0;
    s->col_tgt_ok = 0;
    s->col_si = -1;
    s->col_pitch = s->col_yaw = s->look_drop = s->look_slide[0] = s->look_slide[1] = 0;
    s->locked = 0;
    s->yaw_vel = s->pitch_vel = 0;
    s->cur_dist = row->dist;
    s->yaw = yaw;
    s->pitch = c->pitch_default_deg * DEG;
    s->pivot[0] = pos[0];
    s->pivot[1] = pos[1] + row->pivot_y;
    s->pivot[2] = pos[2];
    s->pivot_vel[0] = s->pivot_vel[1] = s->pivot_vel[2] = 0;
    s->manual_timer = s->idle_pitch_timer = s->claw_t = 0;
    s->reset_t = -1;
    place_eye(s, c, 0, 0);
}

/* looking up must not put the eye under the floor the character stands on (else the cast pins the camera to the
   back of the knight): eye.y >= feet + 0.35 m [придумал] */
static float floor_pitch_min(const CamState *s, const CamInput *in)
{
    float gap = s->pivot[1] - in->char_pos[1] - 0.35f, dist = s->row_dist + s->frame_extra;
    if (gap <= 0) return 0;
    return gap >= dist ? -1.5f : -asinf(gap / dist);
}

void cam_update(CamState *s, const CamCfg *c, const CamInput *in, float dt)
{
    if (in->row) blend_row(s, c, in->row, dt);
    /* lock framing: big or close target -> back off and lift the pivot (§4.7) */
    float want_extra = 0, want_lift = 0;
    if (in->locked) {
        float dx = in->target[0] - in->char_pos[0], dz = in->target[2] - in->char_pos[2];
        if (in->target_radius > 1.5f || dx * dx + dz * dz < 4.0f) {
            want_extra = in->target_radius * 0.5f;
            if (want_extra > 2.0f) want_extra = 2.0f;
            want_lift = 0.3f;
        }
    }
    float kf = dt * 3.0f > 1 ? 1 : dt * 3.0f;
    s->frame_extra += (want_extra - s->frame_extra) * kf;
    s->frame_lift += (want_lift - s->frame_lift) * kf;
    spring(&s->pivot[0], &s->pivot_vel[0], in->char_pos[0], c->omega_h, dt);
    spring(&s->pivot[1], &s->pivot_vel[1], in->char_pos[1] + s->row_pivot_y + s->frame_lift, c->omega_v, dt);
    spring(&s->pivot[2], &s->pivot_vel[2], in->char_pos[2], c->omega_h, dt);
    /* the springs lag ~1 m behind a roll and ~0.6 m vertically down stairs; with the body low in a roll that is
       enough to drop the knight out of the bottom of the frame -> cap the lag (pivot_lag_h / pivot_lag_v) */
    {
        float dx = s->pivot[0] - in->char_pos[0], dz = s->pivot[2] - in->char_pos[2], dh = sqrtf(dx * dx + dz * dz);
        if (dh > c->pivot_lag_h) {
            float k = c->pivot_lag_h / dh;
            s->pivot[0] = in->char_pos[0] + dx * k; s->pivot[2] = in->char_pos[2] + dz * k;
        }
        float ty = in->char_pos[1] + s->row_pivot_y + s->frame_lift, dy = s->pivot[1] - ty;
        if (dy > c->pivot_lag_v) s->pivot[1] = ty + c->pivot_lag_v;
        if (dy < -c->pivot_lag_v) s->pivot[1] = ty - c->pivot_lag_v;
    }

    if (in->locked) {
        if (!s->locked) { s->yaw_vel = s->pitch_vel = 0; s->reset_t = -1; }
        s->locked = 1;
        float tx = in->target[0] - s->pivot[0], ty = in->target[1] - s->pivot[1], tz = in->target[2] - s->pivot[2];
        float hz = sqrtf(tx * tx + tz * tz);
        float want_yaw = atan2f(-tx, -tz);                 /* camera forward (-sin yaw, -cos yaw) -> target */
        float elev = atan2f(-ty, hz > 0.1f ? hz : 0.1f);   /* + = target below the pivot (look down) */
        float want_pitch = c->pitch_default_deg * DEG + s->row_shift * elev;
        float pmin = c->lock_pitch_min_deg * DEG, pmax = c->lock_pitch_max_deg * DEG;
        float pf = floor_pitch_min(s, in);
        if (pmin < pf) pmin = pf;
        if (want_pitch < pmin) want_pitch = pmin;
        if (want_pitch > pmax) want_pitch = pmax;
        spring_angle(&s->yaw, &s->yaw_vel, want_yaw, c->lock_yaw_omega, c->lock_yaw_max_deg * DEG, dt);
        spring_angle(&s->pitch, &s->pitch_vel, want_pitch, c->lock_pitch_omega, 0, dt);
        s->manual_timer = c->autoyaw_delay;
        s->idle_pitch_timer = 0;
        place_eye(s, c, in, dt);
        return;
    }
    s->locked = 0;

    /* 1. manual (claw) */
    if (in->yaw_in != 0 || in->pitch_in != 0) {
        s->claw_t += dt;
        float ramp = s->claw_t >= c->claw_ramp ? 1.0f : s->claw_t / c->claw_ramp;
        s->yaw = wrap_pi(s->yaw - in->yaw_in * c->claw_yaw_rate_deg * DEG * ramp * dt);
        s->pitch += in->pitch_in * c->claw_pitch_rate_deg * DEG * dt;
        s->manual_timer = c->autoyaw_delay;
        s->idle_pitch_timer = 0;
        s->reset_t = -1;
    } else {
        s->claw_t = 0;
        s->idle_pitch_timer += dt;
    }

    /* 2. reset behind the character (facing at press time) */
    if (in->reset) {
        s->reset_t = 0;
        s->reset_from = s->yaw;
        s->reset_to = in->char_facing;
    }
    if (s->reset_t >= 0) {
        s->reset_t += dt;
        float u = s->reset_t >= c->reset_time ? 1.0f : s->reset_t / c->reset_time;
        u = 1.0f - (1.0f - u) * (1.0f - u); /* ease-out */
        s->yaw = wrap_pi(s->reset_from + wrap_pi(s->reset_to - s->reset_from) * u);
        float pd = c->pitch_default_deg * DEG;
        s->pitch += (pd - s->pitch) * u;
        if (s->reset_t >= c->reset_time) { s->reset_t = -1; s->manual_timer = c->autoyaw_delay; }
    } else {
        /* 3. lazy auto-yaw */
        s->manual_timer -= dt;
        if (s->manual_timer <= 0 && in->locomotion && !in->in_action) {
            float d = wrap_pi(in->move_heading - s->yaw), ad = fabsf(d);
            float rate = 0;
            if (ad < c->autoyaw_toward_cam_deg * DEG && ad > c->autoyaw_deadband_deg * DEG)
                rate = c->autoyaw_rate_max_deg * DEG * sinf(ad) * in->speed_frac;
            float step = rate * dt;
            if (step > ad) step = ad;
            s->yaw = wrap_pi(s->yaw + (d > 0 ? step : -step));
        }
        /* 4. pitch back to default */
        if (s->idle_pitch_timer > c->pitch_return_delay) {
            float pd = c->pitch_default_deg * DEG, st = c->pitch_return_rate_deg * DEG * dt, dp = pd - s->pitch;
            s->pitch += dp > st ? st : dp < -st ? -st : dp;
        }
    }
    /* pitch: our + = looking down; rotRangeMinX (-40) = looking up at most 40 deg [версия трактовки, controls §4.2] */
    float pmin = s->row_pitch_min * DEG, pmax = c->pitch_max_deg * DEG, pf = floor_pitch_min(s, in);
    if (pmin < pf) pmin = pf;
    if (s->pitch < pmin) s->pitch = pmin;
    if (s->pitch > pmax) s->pitch = pmax;
    place_eye(s, c, in, dt);
}
