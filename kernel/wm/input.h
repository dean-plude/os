/*
 * input.h — input event queue
 *
 * Phase 8.  A small ring buffer of raw input events produced by the PS/2
 * drivers and consumed by the window-manager event loop.
 */

#pragma once

#include "../include/types.h"

typedef enum {
    INPUT_MOUSE = 1,   /* relative motion + button state */
    INPUT_KEY   = 2,   /* keyboard make/break */
} InputType;

/* Mouse button bitmask */
#define MOUSE_LEFT    (1u << 0)
#define MOUSE_RIGHT   (1u << 1)
#define MOUSE_MIDDLE  (1u << 2)

typedef struct {
    UINT8 type;        /* InputType */
    UINT8 buttons;     /* MOUSE_*  (INPUT_MOUSE) */
    UINT8 scancode;    /* set-1 scancode (INPUT_KEY) */
    UINT8 pressed;     /* 1 = key down, 0 = key up (INPUT_KEY) */
    INT32 dx, dy;      /* relative motion, +x right / +y down (INPUT_MOUSE) */
} InputEvent;

void InputInit(void);
/* Producer (drivers). Drops the event if the queue is full. */
void InputPost(const InputEvent *ev);
/* Consumer (WM). Returns false if empty. */
bool InputPoll(InputEvent *out);
