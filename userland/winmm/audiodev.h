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

/* The devices as winmm numbers them (waveOut/waveIn device IDs): the
 * default first, as on Windows (device 0 is where WAVE_MAPPER plays, and
 * what DRVM_MAPPER_PREFERRED_GET names), then the others oldest first.
 * The IDs move when the default does, as they do on Windows. */
static inline UINT32 audio_wave_devices(int capture, AudioDeviceList *l)
{
    UINT32 n = audio_devices(capture, l);
    for (UINT32 k = 1; k < n; k++) {
        if (!l->dev[k].is_default) continue;
        AudioDeviceList t;
        t.dev[0] = l->dev[k];
        memmove(&l->dev[1], &l->dev[0], k * sizeof(l->dev[0]));
        l->dev[0] = t.dev[0];
        break;
    }
    return n;
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

/* Device @id's endpoint volume (0: the default device's): @v = { left,
 * right (0..65536), mute }; FALSE if there is no such device */
static inline BOOL audio_get_volume(int capture, UINT32 id, UINT32 v[3])
{
    return NtNovaAudioCtl(0, 9, (ULONG_PTR)(capture != 0) | (ULONG_PTR)id << 1, v) == 0;
}
static inline BOOL audio_set_volume(int capture, UINT32 id, const UINT32 v[3])
{
    return NtNovaAudioCtl(0, 8, (ULONG_PTR)(capture != 0) | (ULONG_PTR)id << 1, (void *)v) == 0;
}

/* A device's endpoint ID, as mmdevapi gives it and XAudio2 takes it:
 * "{0.0.0.00000000}.{6e6f7661-6864-6100-0000-0000000000NN}" (0.0.1:
 * capture), NN the device's id.  DirectSound's device GUID is the part in
 * the second braces (the endpoint's AudioEndpoint_GUID), as on Windows. */
static inline void audio_endpoint_id(int capture, UINT32 id, WCHAR out[56])
{
    static const WCHAR base[] = L"{0.0.0.00000000}.{6e6f7661-6864-6100-0000-000000000000}";
    memcpy(out, base, sizeof(base));
    if (capture) out[5] = L'1';
    WCHAR *d = out + 53;                                        /* the last hex digit */
    for (; id; id >>= 4, d--) *d = L"0123456789abcdef"[id & 15];
}

/* The device id in an endpoint ID (or its second half alone, or a GUID
 * string); 0 if it is not one of NovaOS's, @*capture set from the first
 * half when there is one */
static inline UINT32 audio_endpoint_parse(const WCHAR *s, int *capture)
{
    if (!s) return 0;
    if (lstrlenW(s) >= 17 && s[0] == L'{' && s[1] == L'0' && s[2] == L'.' && s[15] == L'}' && s[16] == L'.') {
        if (capture) *capture = s[5] == L'1';
        s += 17;
    }
    static const WCHAR pre[] = L"{6e6f7661-6864-6100-0000-";
    for (int i = 0; pre[i]; i++) {
        WCHAR c = s[i] >= L'A' && s[i] <= L'F' ? s[i] + 32 : s[i];
        if (c != pre[i]) return 0;
    }
    UINT32 id = 0;
    for (int i = 25; i < 37; i++) {
        WCHAR c = s[i];
        int v = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
        if (v < 0) return 0;
        id = id << 4 | (UINT32)v;
    }
    return s[37] == L'}' ? id : 0;
}

/* The same as a GUID: {6E6F7661-6864-6100-0000-<id, 12 hex digits>} */
static inline GUID audio_device_guid(UINT32 id)
{
    GUID g = { 0x6E6F7661, 0x6864, 0x6100, { 0, 0, 0, 0, (BYTE)(id >> 24), (BYTE)(id >> 16), (BYTE)(id >> 8), (BYTE)id } };
    return g;
}
static inline UINT32 audio_guid_device(const GUID *g)
{
    if (g->Data1 != 0x6E6F7661 || g->Data2 != 0x6864 || g->Data3 != 0x6100 || g->Data4[0] || g->Data4[1] ||
        g->Data4[2] || g->Data4[3]) return 0;
    return (UINT32)g->Data4[4] << 24 | (UINT32)g->Data4[5] << 16 | (UINT32)g->Data4[6] << 8 | g->Data4[7];
}

/* What Windows calls a device the kernel names @name: "Speakers (Product)"
 * and "Microphone (Card)" as they are; the sound card's output, which is
 * named after the card, "Speakers (High Definition Audio)" */
static inline void audio_friendly_name(int capture, const char *name, WCHAR *out, int n)
{
    BOOL paren = FALSE;
    for (const char *p = name; *p && !paren; p++) paren = p[0] == ' ' && p[1] == '(';
    if (!paren) name = capture ? "Microphone (High Definition Audio)" : "Speakers (High Definition Audio)";
    int i = 0;
    for (; name[i] && i < n - 1; i++) out[i] = (WCHAR)(BYTE)name[i];
    out[i] = 0;
}
