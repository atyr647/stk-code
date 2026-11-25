/* Controller sampling for the SDL back-end (sdl_n64.c).
 *
 * The controllers are sampled at every vertical blank (60 Hz, from the
 * video interrupt, where libdragon itself reads them), not once a frame,
 * and every change becomes an event, in order: at the frame rates of a race
 * or of a screen being paged in (under 1 frame/s at worst) many taps can
 * fall within one frame, and reading the state once a frame would see one
 * of them, or none.
 *
 * This file is part of the platform library because that is resident: code
 * called from an interrupt must not be paged (n64/compat/src/pager.c), and
 * the SDL back-end is. With the sampling in the back-end the game hung at
 * random while loading, whenever the handler's block had been evicted.
 *
 * (A thread for the sampling was tried before the interrupt: preempting the
 * game 60 times a second tripped an assertion in libdragon's kernel
 * mutexes.) Only the handler calls joypad_poll(); stick motion is merged
 * while it stays on the same side of the point where STK counts it as
 * pushed, so a stick moved slowly does not fill the queue.
 */
#include <libdragon.h>
#include <string.h>
#include "pad_n64.h"

#define PAD_QUEUE 256
#define PAD_ZONE  21845             /* STK counts a stick pushed past 2/3 */

static stk_n64_pad_event_t g_queue[PAD_QUEUE];
static volatile unsigned g_head, g_tail;
static joypad_inputs_t g_last[JOYPAD_PORT_COUNT];
static uint8_t g_connected[JOYPAD_PORT_COUNT];
static int g_started;

static uint32_t pad_bits(const joypad_inputs_t *in)
{
    const unsigned b[STK_N64_PAD_BUTTONS] = { in->btn.a, in->btn.b, in->btn.z,
        in->btn.start, in->btn.l, in->btn.r, in->btn.c_up, in->btn.c_down,
        in->btn.c_left, in->btn.c_right };
    uint32_t m = 0;
    for (int i = 0; i < STK_N64_PAD_BUTTONS; i++)
        if (b[i]) m |= 1u << i;
    return m;
}

/* SDL's hat bits: up 1, right 2, down 4, left 8 */
static unsigned pad_hat(const joypad_inputs_t *in)
{
    return (in->btn.d_up ? 1 : 0) | (in->btn.d_right ? 2 : 0) |
           (in->btn.d_down ? 4 : 0) | (in->btn.d_left ? 8 : 0);
}

/* an N64 stick reaches about +-80; SDL's range, Y pointing down */
static int16_t axis_value(int8_t v, int invert)
{
    int s = (int)v * 32767 / 80;
    if (invert) s = -s;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

static int pad_zone(int v) { return v > PAD_ZONE ? 1 : v < -PAD_ZONE ? -1 : 0; }

static void pad_push(int kind, int port, int index, int value)
{
    if (g_tail - g_head < PAD_QUEUE)
    {
        stk_n64_pad_event_t *e = &g_queue[g_tail++ % PAD_QUEUE];
        e->kind = kind; e->port = port; e->index = index; e->value = value;
    }
}

static void pad_vblank(void *arg)
{
    (void)arg;
    joypad_poll();
    int index = 0;
    for (int p = 0; p < JOYPAD_PORT_COUNT; p++)
    {
        const int is = joypad_is_connected(p);
        if (is && !g_connected[p])
        {
            pad_push(STK_N64_PAD_ADDED, p, index, 0);
            memset(&g_last[p], 0, sizeof(g_last[p]));
        }
        else if (!is && g_connected[p])
            pad_push(STK_N64_PAD_REMOVED, p, 0, 0);
        g_connected[p] = is;
        if (!is)
            continue;
        index++;
        joypad_inputs_t in = joypad_get_inputs(p);
        /* The D-pad moves the stick's axes to their ends: STK binds
         * steering and menu navigation to one input, the stick when a
         * controller has one, and the D-pad would do nothing. (It is still
         * reported as a hat as well, for a player who binds it.) */
        if (in.btn.d_left)  in.stick_x = -85;
        if (in.btn.d_right) in.stick_x = 85;
        if (in.btn.d_down)  in.stick_y = -85;
        if (in.btn.d_up)    in.stick_y = 85;
        const joypad_inputs_t old = g_last[p];
        g_last[p] = in;

        const uint32_t now_b = pad_bits(&in), old_b = pad_bits(&old);
        for (int b = 0; b < STK_N64_PAD_BUTTONS; b++)
            if ((now_b ^ old_b) & (1u << b))
                pad_push(STK_N64_PAD_BUTTON, p, b, (now_b >> b) & 1);

        const int8_t axes[2] = { in.stick_x, in.stick_y };
        const int8_t old_axes[2] = { old.stick_x, old.stick_y };
        for (int a = 0; a < 2; a++)
        {
            if (axes[a] == old_axes[a])
                continue;
            const int v = axis_value(axes[a], a == 1);
            /* merge with a queued motion of this axis in the same zone */
            if (g_tail != g_head)
            {
                stk_n64_pad_event_t *q = &g_queue[(g_tail - 1) % PAD_QUEUE];
                if (q->kind == STK_N64_PAD_AXIS && q->port == p &&
                    q->index == a && pad_zone(q->value) == pad_zone(v))
                {
                    q->value = v;
                    continue;
                }
            }
            pad_push(STK_N64_PAD_AXIS, p, a, v);
        }
        const unsigned hat = pad_hat(&in), old_hat = pad_hat(&old);
        if (hat != old_hat)
            pad_push(STK_N64_PAD_HAT, p, 0, hat);
    }
}

/* Called from the game loop's first poll, not when the controllers are
 * initialised: libdragon identifies the controllers' accessories in the
 * first moments after joypad_init(). */
void stk_n64_pad_start(void)
{
    if (g_started)
        return;
    g_started = 1;
    vi_install_vblank_handler(pad_vblank, NULL);
}

int stk_n64_pad_started(void) { return g_started; }

int stk_n64_pad_next(stk_n64_pad_event_t *e)
{
    disable_interrupts();
    const int any = g_tail != g_head;
    if (any)
        *e = g_queue[g_head++ % PAD_QUEUE];
    enable_interrupts();
    return any;
}

int16_t stk_n64_pad_axis(int port, int axis)
{
    disable_interrupts();
    const joypad_inputs_t in = g_last[port];
    enable_interrupts();
    return axis == 0 ? axis_value(in.stick_x, 0) :
           axis == 1 ? axis_value(in.stick_y, 1) : 0;
}
