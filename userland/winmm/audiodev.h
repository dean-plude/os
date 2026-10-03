/*
 * audiodev.h — the sound devices NovaOS has, for winmm's device IDs and
 * mmdevapi's endpoints (NtNovaAudioCtl ops 10-12, kernel/um/um_audio.c)
 *
 * The kernel lists the attached outputs or inputs oldest first, each with
 * an id that stays the same while it is attached; one of each is the
 * default (the newest, or the one chosen in Settings).  A stream plays on
 * (records from) the default unless it is routed to a device by id.
 */

#pragma once

#include <windows.h>
#include <winternl.h>

#define AUDIO_MAX_DEVICES 8

typedef struct {
    UINT32 count;
    struct { UINT32 id, is_default; char name[96]; } dev[AUDIO_MAX_DEVICES];
} AudioDeviceList;

/* The outputs (@capture: inputs) attached now; how many */
static inline UINT32 audio_devices(int capture, AudioDeviceList *l)
{
    memset(l, 0, sizeof(*l));
    if (NtNovaAudioCtl(0, 10, (ULONG_PTR)(capture != 0), l) != 0 || l->count > AUDIO_MAX_DEVICES) l->count = 0;
    return l->count;
}

/* The id of device @index (oldest first) of a direction; 0 if none */
static inline UINT32 audio_device_id(int capture, UINT index)
{
    AudioDeviceList l;
    return index < audio_devices(capture, &l) ? l.dev[index].id : 0;
}

/* Play stream @s on (record it from) device @id (0: the default) */
static inline BOOL audio_route(INT_PTR s, UINT32 id)
{
    return NtNovaAudioCtl(s, 12, id, 0) == 0;
}
