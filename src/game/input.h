/*
 * Input layer (the controls design notes §3, §4.1, §5): raw PSP pad -> logical actions. Gameplay never sees buttons.
 *   Scheme A (default): stick move, X tap = dodge (on release, < 300 ms), X hold = run, [] light, /\ heavy,
 *                       R guard, L tap = lock-on / camera behind (no targets yet -> always camera reset),
 *                       "claw" d-pad camera = option, off by default.
 *   Scheme B:          O tap = dodge / O hold = run, R light, L tap (< 180 ms) = lock / camera reset,
 *                       d-pad up = camera reset.
 *   SELECT + START toggles the scheme (debug).
 * Stick: radial dead zone 32/128 with 4-unit hysteresis, saturation 110, curve n^1.5, centre calibration over the
 * first 16 frames. All numbers in InputCfg.
 */
#ifndef ASH_INPUT_H
#define ASH_INPUT_H

#include <psptypes.h>

enum {
    ACT_LIGHT = 1u << 0, ACT_HEAVY = 1u << 1, ACT_GUARD = 1u << 2, ACT_PARRY = 1u << 3, ACT_DODGE = 1u << 4,
    ACT_RUN = 1u << 5, ACT_ITEM = 1u << 6, ACT_INTERACT = 1u << 7, ACT_LOCK = 1u << 8, ACT_CAM_RESET = 1u << 9,
    ACT_MENU = 1u << 10, ACT_SCHEME = 1u << 11,
};

typedef struct {
    float dz_in, dz_out;      /* radial dead zone enter / leave (raw units of 128) */
    float sat;                /* raw radius treated as full deflection */
    float curve;              /* exponent */
    float walk_max, jog_min;  /* walk/jog hysteresis on the curved magnitude */
    float tap_ms;             /* dodge vs run threshold (A: X, B: O) */
    float lock_tap_ms;        /* scheme B: L tap vs guard */
    int claw;                 /* scheme A: d-pad turns the camera */
    int calib_frames;
} InputCfg;

typedef struct {
    u32 buttons;              /* PSP_CTRL_* */
    u8 lx, ly;
} RawPad;

typedef struct {
    float move_x, move_y;     /* after dead zone + curve, -1..1, y < 0 = up (stick space) */
    float move_mag;
    int jog;                  /* move_mag past the walk/jog hysteresis */
    u32 pressed, held, released;
    float cam_yaw_in, cam_pitch_in;
} AshInput;

typedef struct {
    InputCfg cfg;
    int scheme;               /* 0 = A, 1 = B */
    float cx, cy;
    int calib_n;
    float calib_sx, calib_sy;
    int in_dz;                /* hysteresis state */
    int jog;
    u32 prev_buttons;
    float dodge_held_ms, lock_held_ms;
    int run_latched;
    u32 prev_held;
} InputState;

InputCfg input_default_cfg(void);
void input_init(InputState *s, int scheme);
void input_update(InputState *s, const RawPad *pad, float dt, AshInput *out);
const char *input_scheme_name(const InputState *s);

#endif
