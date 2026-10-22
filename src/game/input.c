#include "input.h"

#include <math.h>
#include <pspctrl.h>
#include <string.h>

InputCfg input_default_cfg(void)
{
    InputCfg c;
    c.dz_in = 32.0f;
    c.dz_out = 28.0f;
    c.sat = 110.0f;
    c.curve = 1.5f;
    c.walk_max = 0.70f;
    c.jog_min = 0.75f;
    c.tap_ms = 300.0f;
    c.lock_tap_ms = 180.0f;
    c.claw = 0;
    c.calib_frames = 16;
    return c;
}

void input_init(InputState *s, int scheme)
{
    memset(s, 0, sizeof(*s));
    s->cfg = input_default_cfg();
    s->scheme = scheme;
    s->cx = s->cy = 128.0f;
    s->in_dz = 1;
}

const char *input_scheme_name(const InputState *s) { return s->scheme ? "B" : "A"; }

static void stick(InputState *s, const RawPad *pad, AshInput *o)
{
    const InputCfg *c = &s->cfg;
    if (s->calib_n < c->calib_frames) { /* centre calibration while nothing is pressed */
        if (pad->buttons == 0) {
            s->calib_sx += pad->lx; s->calib_sy += pad->ly; s->calib_n++;
            if (s->calib_n == c->calib_frames) {
                float mx = s->calib_sx / s->calib_n, my = s->calib_sy / s->calib_n;
                if (fabsf(mx - 128) <= 12 && fabsf(my - 128) <= 12) { s->cx = mx; s->cy = my; }
            }
        }
    }
    float x = pad->lx - s->cx, y = pad->ly - s->cy;
    float r = sqrtf(x * x + y * y);
    float lim = s->in_dz ? c->dz_in : c->dz_out;
    if (r < lim) {
        s->in_dz = 1;
        o->move_x = o->move_y = o->move_mag = 0;
        s->jog = 0;
    } else {
        s->in_dz = 0;
        float n = (r - c->dz_in) / (c->sat - c->dz_in);
        n = n < 0 ? 0 : n > 1 ? 1 : n;
        n = powf(n, c->curve);
        if (n < 0.02f) n = 0.02f; /* out of the dead zone = moving */
        o->move_x = x / r * n;
        o->move_y = y / r * n;
        o->move_mag = n;
        if (s->jog && n < c->walk_max) s->jog = 0;
        else if (!s->jog && n > c->jog_min) s->jog = 1;
    }
    o->jog = s->jog;
}

void input_update(InputState *s, const RawPad *pad, float dt, AshInput *o)
{
    memset(o, 0, sizeof(*o));
    stick(s, pad, o);
    const u32 b = pad->buttons, pressed = b & ~s->prev_buttons, released = ~b & s->prev_buttons;
    s->prev_buttons = b;
    const float ms = dt * 1000.0f;
    u32 held = 0, act_pressed = 0;

    if ((b & PSP_CTRL_SELECT) && (pressed & PSP_CTRL_START)) {
        s->scheme = !s->scheme;
        act_pressed |= ACT_SCHEME;
    } else if ((pressed & PSP_CTRL_START) && !(b & PSP_CTRL_SELECT)) {
        act_pressed |= ACT_MENU;
    }

    /* dodge / run on one button: decided on release (< tap_ms = dodge) or by hold time (run) */
    u32 dodge_btn = s->scheme ? PSP_CTRL_CIRCLE : PSP_CTRL_CROSS;
    if (b & dodge_btn) {
        s->dodge_held_ms += ms;
        if (s->dodge_held_ms >= s->cfg.tap_ms) s->run_latched = 1;
    }
    if (s->run_latched && (b & dodge_btn)) held |= ACT_RUN;
    if (released & dodge_btn) {
        if (!s->run_latched) act_pressed |= ACT_DODGE;
        s->dodge_held_ms = 0;
        s->run_latched = 0;
    }

    if (s->scheme == 0) {
        if (pressed & PSP_CTRL_SQUARE) act_pressed |= ACT_LIGHT;
        if (pressed & PSP_CTRL_TRIANGLE) act_pressed |= ACT_HEAVY;
        if (b & PSP_CTRL_RTRIGGER) held |= ACT_GUARD;
        if (pressed & PSP_CTRL_LTRIGGER) act_pressed |= ACT_LOCK; /* no targets yet -> camera behind */
        if (pressed & PSP_CTRL_CIRCLE) act_pressed |= ACT_ITEM;
        if (pressed & PSP_CTRL_UP) act_pressed |= ACT_INTERACT;
        if (s->cfg.claw) {
            o->cam_yaw_in = (b & PSP_CTRL_RIGHT) ? 1.0f : (b & PSP_CTRL_LEFT) ? -1.0f : 0.0f;
            o->cam_pitch_in = (b & PSP_CTRL_DOWN) ? 1.0f : (b & PSP_CTRL_UP) ? -1.0f : 0.0f;
        }
    } else {
        if ((pressed & PSP_CTRL_RTRIGGER) && (b & PSP_CTRL_LTRIGGER)) act_pressed |= ACT_PARRY;
        else if (pressed & PSP_CTRL_RTRIGGER) act_pressed |= ACT_LIGHT;
        if (pressed & PSP_CTRL_TRIANGLE) act_pressed |= ACT_HEAVY;
        if (b & PSP_CTRL_LTRIGGER) { held |= ACT_GUARD; s->lock_held_ms += ms; }
        if (released & PSP_CTRL_LTRIGGER) {
            if (s->lock_held_ms < s->cfg.lock_tap_ms) act_pressed |= ACT_LOCK;
            s->lock_held_ms = 0;
        }
        if (pressed & PSP_CTRL_SQUARE) act_pressed |= ACT_ITEM;
        if (pressed & PSP_CTRL_CROSS) act_pressed |= ACT_INTERACT;
        if (pressed & PSP_CTRL_UP) act_pressed |= ACT_CAM_RESET;
    }
    o->pressed = act_pressed;
    o->held = held;
    o->released = s->prev_held & ~held;
    s->prev_held = held;
}
