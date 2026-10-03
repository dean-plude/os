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
    INPUT_TOUCH = 3,   /* a touch contact: where it is, or that it lifted; or a frame's end */
    INPUT_PEN   = 4,   /* a pen tablet's report (tablet.h): where, pressure, buttons; it does
                        * not move the pointer (the device posts INPUT_MOUSE with from_pen
                        * for that too, right after it, while the pen is in range) */
} InputType;

#define TOUCH_MAX     10   /* contacts tracked at once (slots 0-9) */
#define TOUCH_FRAME   0xFF /* InputEvent.contact of the event that ends a frame */

/* Mouse button bitmask */
#define MOUSE_LEFT    (1u << 0)
#define MOUSE_RIGHT   (1u << 1)
#define MOUSE_MIDDLE  (1u << 2)
#define MOUSE_X1      (1u << 3)   /* the side buttons: back */
#define MOUSE_X2      (1u << 4)   /*   and forward */

typedef struct {
    UINT8 type;        /* InputType */
    UINT8 buttons;     /* MOUSE_*  (INPUT_MOUSE) */
    UINT8 scancode;    /* set-1 scancode (INPUT_KEY) */
    UINT8 pressed;     /* 1 = key down, 0 = key up (INPUT_KEY) */
    UINT8 extended;    /* 1 = E0-prefixed key: arrows, Delete, Win... */
    INT32 dx, dy;      /* relative motion, +x right / +y down (INPUT_MOUSE) */
    INT32 dz;          /* wheel: +1 per notch away from the user (INPUT_MOUSE) */
    UINT8 absolute;    /* dx, dy are a position: 0-65535 across the screen (tablets, touch) */
    INT32 dw;          /* horizontal wheel: +1 per notch to the right (INPUT_MOUSE) */
    UINT8 contact;     /* INPUT_TOUCH: the contact's slot (0..TOUCH_MAX-1), or TOUCH_FRAME:
                        * every contact of the frame (one device report) has been posted.
                        * dx, dy: where (0-65535 across the screen); pressed: touching */
    UINT16 pressure;   /* INPUT_PEN: 0..TABLET_PRESSURE; dx, dy absolute; buttons: bit 0 the
                        * tip, 1-2 the barrel buttons; pressed: in range; extended: eraser */
    INT16  tilt_x, tilt_y; /* INPUT_PEN: tenths of a degree from upright, -900..900: + is the
                        * pen's top leaning right (x) or toward the user (y), as Windows' */
    UINT16 twist;      /* INPUT_PEN: barrel rotation, tenths of a degree clockwise, 0..3599 */
    UINT8  pen_has;    /* INPUT_PEN: PEN_HAS_*, what the device reports (others are 0) */
    UINT8  from_pen;   /* INPUT_MOUSE: a pen's motion and tip; its INPUT_PEN came just before
                        * (programs get WM_POINTER* for it, user32's pointer.c) */
} InputEvent;

/* InputEvent.pen_has */
#define PEN_HAS_TILT  1
#define PEN_HAS_TWIST 2

/* -----------------------------------------------------------------------
 * Keyboard translation
 * ----------------------------------------------------------------------- */

/* Set-1 scancodes (extended ones need InputEvent.extended / KeyEvent.extended) */
#define KEY_ESC        0x01
#define KEY_BACKSPACE  0x0E
#define KEY_TAB        0x0F
#define KEY_ENTER      0x1C
#define KEY_CTRL       0x1D
#define KEY_LSHIFT     0x2A
#define KEY_RSHIFT     0x36
#define KEY_ALT        0x38
#define KEY_SPACE      0x39
#define KEY_CAPSLOCK   0x3A
#define KEY_NUMLOCK    0x45
#define KEY_SCROLLLOCK 0x46
#define KEY_F4         0x3E
#define KEY_F12        0x58
#define KEY_HOME       0x47   /* extended */
#define KEY_UP         0x48   /* extended */
#define KEY_PGUP       0x49   /* extended */
#define KEY_LEFT       0x4B   /* extended */
#define KEY_RIGHT      0x4D   /* extended */
#define KEY_END        0x4F   /* extended */
#define KEY_DOWN       0x50   /* extended */
#define KEY_PGDN       0x51   /* extended */
#define KEY_INSERT     0x52   /* extended */
#define KEY_DELETE     0x53   /* extended */
#define KEY_LWIN       0x5B   /* extended */
/* Media, browser and launch keys (all extended), as PS/2 keyboards send
 * them and USB consumer-control usages are translated to */
#define KEY_PREV_TRACK 0x10
#define KEY_NEXT_TRACK 0x19
#define KEY_MUTE       0x20
#define KEY_CALCULATOR 0x21
#define KEY_PLAY_PAUSE 0x22
#define KEY_MEDIA_STOP 0x24
#define KEY_VOL_DOWN   0x2E
#define KEY_VOL_UP     0x30
#define KEY_WWW_HOME   0x32
#define KEY_POWER      0x5E
#define KEY_SLEEP      0x5F
#define KEY_WAKE       0x63
#define KEY_WWW_SEARCH 0x65
#define KEY_WWW_FAVORITES 0x66
#define KEY_WWW_REFRESH 0x67
#define KEY_WWW_STOP   0x68
#define KEY_WWW_FORWARD 0x69
#define KEY_WWW_BACK   0x6A
#define KEY_MY_COMPUTER 0x6B
#define KEY_MAIL       0x6C
#define KEY_MEDIA_SELECT 0x6D

typedef struct {
    UINT8 scancode;
    bool  extended;
    bool  pressed;
    bool  shift, ctrl, alt;
    bool  altgr;       /* AltGr held (right Alt, or Ctrl+Alt, in a layout that has it) */
    char  ch;          /* printable ASCII, '\n', '\t', '\b', or 0 */
    UINT16 wch;        /* the character the key types in the user's layout (UTF-16: ch, or
                        * a letter like ä or é; 0 for a dead key, whose accent goes on
                        * the next letter); with an AltGr character ctrl and alt are false */
} KeyEvent;

/* Update modifier state from a key event and translate it.  Returns false
 * for events that are not keys. */
bool InputTranslateKey(const InputEvent *ev, KeyEvent *out);

void InputInit(void);
/* Modifier keys held: bit 0 Shift, 1 Ctrl, 2 Alt; bit 3 Caps Lock on */
UINT32 InputModifiers(void);
/* Lock keys on, as keyboard LEDs show them: bit 0 Num Lock, 1 Caps Lock,
 * 2 Scroll Lock */
UINT32 InputLockState(void);
/* Producer (drivers). Drops the event if the queue is full. */
void InputPost(const InputEvent *ev);
/* Consumer (WM). Returns false if empty. */
bool InputPoll(InputEvent *out);
/* A touch screen with @contacts contacts was found (drivers); and the most
 * contacts any touch screen found has (0: none) */
void   InputTouchScreen(int contacts);
int    InputTouchContacts(void);
