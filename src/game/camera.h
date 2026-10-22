/*
 * Third-person camera for the PSP's single stick, the controls design notes §4.2-4.7: free camera, lock-on camera, collision.
 *   - pivot = feet + pivot_y, critically damped spring (horizontal 10 rad/s, vertical 6 rad/s), lag capped at
 *     0.5 m / 0.15 m (rolls, stairs);
 *   - lazy auto-yaw while walking/running: rate = autoyaw_rate_max * sin|d| * speed_frac, dead band 10 deg,
 *     no turning when running toward the camera (|d| > 120 deg), off during rolls/attacks and for autoyaw_delay
 *     after manual input or a reset;
 *   - reset behind the character (L tap with no target): ease-out over reset_time, to the facing at press time;
 *   - optional "claw" (d-pad) manual yaw/pitch.
 *   - lock-on (§4.7): yaw/pitch springs toward the target (lockRotXShiftRatio of its elevation), wider framing for a
 *     big target (dist + min(2, radius / 2), pivot + 0.3 m);
 *   - LockCamParam row (dist, pivot height, fov, pitch min, lock shift) comes in CamInput every frame (row of the floor
 *     collision under the player), blended over ~0.5 s;
 *   - collision (§4.5): sphere cast pivot -> eye through CamInput.cast; pull-in instant, push-out col_out_speed m/s;
 *     a wall right behind the character (a lock-on across a corridor, a roll into a wall) would leave the eye 0.8 m
 *     behind a pivot 1.3-1.7 m above the feet, so the body drops out of the 43 deg frame: the camera then climbs
 *     (extra pitch, first 20/40/60 deg step with col_escape_dist free) and lowers the look-at point to close_look_y
 *     above the feet as it gets closer than close_dist, locked - also toward the target (<= close_lock_slide, <= 0.4 * distance) [придумал, bug "camera loses the player", 14.10].
 * Defaults = LockCamParam row 0 of DSR (dist 4.0 m, pivot 1.42 m, fovY 43 deg, pitch min -40 deg).
 */
#ifndef ASH_CAMERA_H
#define ASH_CAMERA_H

typedef struct {
    float dist, pivot_y, fov_deg;              /* LockCamParam row */
    float pitch_min_deg, pitch_max_deg, pitch_default_deg;
    float omega_h, omega_v;                    /* pivot spring, rad/s */
    float pivot_lag_h, pivot_lag_v;            /* m, max spring lag behind the character */
    float autoyaw_delay, autoyaw_rate_max_deg, autoyaw_deadband_deg, autoyaw_toward_cam_deg;
    float reset_time;
    float claw_yaw_rate_deg, claw_pitch_rate_deg, claw_ramp;
    float pitch_return_delay, pitch_return_rate_deg;
    float col_radius, col_min_dist, col_out_speed;            /* §4.5 */
    float col_escape_dist, col_escape_step_deg, col_escape_max_deg, col_escape_rate_deg; /* climb over a wall */
    float col_side_step_deg, col_side_max_deg, col_side_rate_deg;  /* swing aside along a wall the climb cannot clear */
    float col_search_hold;     /* s, keep a climb/swing answer this long before searching again (1 cast/step meanwhile) */
    int col_search_probes;     /* climb/swing probes a step at most (a search spreads over steps) */
    float close_dist, close_look_y, close_lock_slide;         /* pulled in: look at the body / toward the target */
    float lock_yaw_omega, lock_yaw_max_deg, lock_pitch_omega;  /* §4.7 */
    float lock_pitch_min_deg, lock_pitch_max_deg;
    float lock_pitch_shift;                                    /* lockRotXShiftRatio */
    float row_blend_time;
} CamCfg;

/* One LockCamParam row (DSR GameParam, the controls design notes §2.1). */
typedef struct { int id; float dist, pivot_y, pitch_min_deg, lock_shift, lock_radius, fov_deg; } CamRow;
const CamRow *cam_row(int id); /* unknown / -1 -> row 0 */

typedef float (*CamCastFn)(void *ctx, const float a[3], const float b[3], float r); /* free fraction 0..1 */

typedef struct {
    float yaw, pitch;          /* rad; look direction (-sin yaw cos pitch, -sin pitch, -cos yaw cos pitch) */
    float pivot[3], pivot_vel[3];
    float manual_timer, idle_pitch_timer, claw_t;
    float reset_t, reset_from, reset_to;       /* reset_t < 0 = none */
    float eye[3], at[3];
    float cur_dist;            /* after collision */
    float row_dist, row_pivot_y, row_fov, row_pitch_min, row_shift;  /* blended row */
    float frame_extra, frame_lift;                                   /* lock framing, blended */
    int locked;
    float yaw_vel, pitch_vel;
    float hit_frac;            /* last cast result, 1 = free */
    float col_pitch;           /* rad, extra pitch while the space behind is blocked (camera climbs over the wall) */
    float col_yaw;             /* rad, extra yaw while a tall wall right behind blocks the climb (camera swings aside) */
    float col_hold, col_tgt_cy, col_tgt_cp;  /* last climb/swing search: hold time left, its answer */
    int col_tgt_ok;            /* that answer was a free direction (else the freest of the blocked ones) */
    int col_si;                /* search in progress: next probe index, -1 = none */
    float col_s_first, col_s_lean, col_s_best, col_s_cy, col_s_cp;  /* that search: start swing/side, best so far */
    float look_drop;           /* m, look-at point lowered from the pivot while the camera is pulled in */
    float look_slide[2];       /* m (x, z), look-at moved toward the lock target while pulled in */
} CamState;

typedef struct {
    float char_pos[3];         /* feet */
    float char_facing;         /* rad */
    float move_heading;        /* rad, where the character is running */
    float speed_frac;          /* 0..1 */
    int locomotion;            /* walk / jog / run */
    int in_action;             /* roll / attack: no auto-yaw */
    int reset;                 /* ACT_LOCK with no target, or ACT_CAM_RESET */
    float yaw_in, pitch_in;    /* claw, -1..1 */
    const CamRow *row;         /* LockCamParam row under the player */
    int locked;
    float target[3];           /* lock point */
    float target_radius;
    CamCastFn cast;            /* NULL = no collision */
    void *cast_ctx;
} CamInput;

CamCfg cam_default_cfg(void);
void cam_reset(CamState *s, const CamCfg *c, const CamRow *row, const float pos[3], float yaw);
void cam_update(CamState *s, const CamCfg *c, const CamInput *in, float dt);

#endif
