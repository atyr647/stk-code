/* Controller events from the vertical blank handler (pad_n64.c) */
#ifndef STK_N64_PAD_H
#define STK_N64_PAD_H
#include <stdint.h>

#define STK_N64_PAD_BUTTONS 10   /* A B Z Start L R C-up C-down C-left C-right */

enum { STK_N64_PAD_ADDED, STK_N64_PAD_REMOVED, STK_N64_PAD_BUTTON,
       STK_N64_PAD_AXIS, STK_N64_PAD_HAT };

typedef struct
{
    uint8_t kind, port, index;   /* index: device index, button, axis */
    int16_t value;               /* button: 1 down; axis: SDL range; hat bits */
} stk_n64_pad_event_t;

void    stk_n64_pad_start(void);
int     stk_n64_pad_started(void);
int     stk_n64_pad_next(stk_n64_pad_event_t *e);
int16_t stk_n64_pad_axis(int port, int axis);
#endif
