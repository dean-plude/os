/*
 * um_audio.c — the kernel half of winmm and mmdevapi: playback and
 * recording streams
 *
 * Each stream is a UO_AUDIO handle object wrapping a mixer stream
 * (drivers/audio.c), so closing the handle, or the program ending, stops
 * its sound.  Streams take 48 kHz s16 stereo frames; the DLLs convert.
 * The services are NovaOS-private: Windows' audio stack talks to the
 * AudioSrv service and kernel-streaming drivers, which NovaOS omits.
 */

#include "um_internal.h"
#include "../ke/probe.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../drivers/audio.h"
#include "../ke/printf.h"

#define BOUNCE_FRAMES 4096

static void audio_destroy(UmObject *o) { AudioClose(o->audio); }

static int handle_stream(UINT64 h)
{
    UmObject *o = um_handle_object(UmCurrent(), h, UO_AUDIO);
    if (!o) return -1;
    int s = o->audio;
    um_ob_unref(o);
    return s;
}

#define OPEN_CAPTURE 0x80000000u

/* NtNovaAudioOpen(frames): a paused stream that can queue @frames
 * (0: the default); with OPEN_CAPTURE set in @frames, a recording stream.
 * Returns a handle, or 0 (no device, or none free). */
static UINT64 sys_open(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    int s = AudioOpen((UINT32)a1 & ~OPEN_CAPTURE, (a1 & OPEN_CAPTURE) != 0);
    if (s < 0) return 0;
    UmObject *o = kzalloc(sizeof(*o));
    if (!o) { AudioClose(s); return 0; }
    o->type = UO_AUDIO;
    o->refs = 1;
    o->audio = s;
    o->destroy = audio_destroy;
    UINT64 hv = um_handle_new_object(UmCurrent(), o);
    um_ob_unref(o);
    return hv;
}

/* NtNovaAudioWrite(h, frames, n): queue up to @n frames.  Returns how many
 * fit, or -1 for a bad handle or buffer. */
static UINT64 sys_write(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    int s = handle_stream(a1);
    if (s < 0) return (UINT64)(INT64)-1;
    INT16 *tmp = kmalloc(BOUNCE_FRAMES * 4);
    if (!tmp) return 0;
    UINT64 done = 0, n = a3;
    while (done < n) {
        UINT32 m = n - done > BOUNCE_FRAMES ? BOUNCE_FRAMES : (UINT32)(n - done);
        if (!NT_SUCCESS(CopyFromUser(tmp, (const void *)(uintptr_t)(a2 + done * 4), (size_t)m * 4))) {
            kfree(tmp);
            return done ? done : (UINT64)(INT64)-1;
        }
        UINT32 got = AudioWrite(s, tmp, m);
        done += got;
        if (got < m) break;                          /* full */
    }
    kfree(tmp);
    return done;
}

/* NtNovaAudioCtl(h, op, arg, out):
 *   0 status -> AudioStatus     1 run (arg 1) / pause (arg 0)
 *   2 flush                      3 set volume (arg: left | right << 16, 0..0xFFFF each)
 *   4 get volume -> UINT32[2] (0..65536)  5 the device (h unused) -> { UINT32 present, rate; char name[96]; }
 *   6 read up to @arg recorded frames into @out (a capture stream): returns how many
 *   7 the recording device (h unused) -> { UINT32 present, rate; char name[96]; }
 *   8 set the endpoint volume (h unused, arg = capture) from @out: UINT32[3] { left, right (0..65536), mute }
 *   9 get the endpoint volume (h unused, arg = capture) -> UINT32[3] { left, right (0..65536), mute }
 * Returns 0, or -1 for a bad handle or buffer. */
static UINT64 sys_ctl(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    if (a2 == 7) {
        struct { UINT32 present, rate; char name[96]; } info;
        memset(&info, 0, sizeof(info));
        info.present = AudioCanRecord();
        info.rate = AUDIO_RATE;
        if (info.present) ksnprintf(info.name, sizeof(info.name), "%s", AudioInputName());
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, &info, sizeof(info))) ? 0 : (UINT64)(INT64)-1;
    }
    if (a2 == 8) {
        UINT32 v[3];
        if (!NT_SUCCESS(CopyFromUser(v, (const void *)(uintptr_t)a4, sizeof(v)))) return (UINT64)(INT64)-1;
        AudioSetMaster((int)(a3 & 1), v[0] > 65536 ? 65536 : v[0], v[1] > 65536 ? 65536 : v[1], v[2] != 0);
        return 0;
    }
    if (a2 == 9) {
        UINT32 v[3];
        bool mute;
        AudioGetMaster((int)(a3 & 1), &v[0], &v[1], &mute);
        v[2] = mute;
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, v, sizeof(v))) ? 0 : (UINT64)(INT64)-1;
    }
    if (a2 == 5) {
        struct { UINT32 present, rate; char name[96]; } info;
        memset(&info, 0, sizeof(info));
        info.present = AudioPresent();
        info.rate = AUDIO_RATE;
        strncpy(info.name, AudioDeviceName(), sizeof(info.name) - 1);
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, &info, sizeof(info))) ? 0 : (UINT64)(INT64)-1;
    }
    int s = handle_stream(a1);
    if (s < 0) return (UINT64)(INT64)-1;
    switch (a2) {
    case 0: {
        AudioStatus st;
        if (!AudioGetStatus(s, &st)) return (UINT64)(INT64)-1;
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, &st, sizeof(st))) ? 0 : (UINT64)(INT64)-1;
    }
    case 1: AudioRun(s, a3 != 0); return 0;
    case 6: {
        INT16 *tmp = kmalloc(BOUNCE_FRAMES * 4);
        if (!tmp) return 0;
        UINT64 done = 0;
        while (done < a3) {
            UINT32 m = a3 - done > BOUNCE_FRAMES ? BOUNCE_FRAMES : (UINT32)(a3 - done);
            UINT32 got = AudioRead(s, tmp, m);
            if (!got) break;
            if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)(a4 + done * 4), tmp, (size_t)got * 4))) {
                kfree(tmp);
                return (UINT64)(INT64)-1;               /* (those frames are lost) */
            }
            done += got;
            if (got < m) break;
        }
        kfree(tmp);
        return done;
    }
    case 2: AudioFlush(s); return 0;
    case 3: {
        UINT32 l = (UINT32)(a3 & 0xFFFF), r = (UINT32)((a3 >> 16) & 0xFFFF);
        AudioSetVolume(s, l + (l >> 15), r + (r >> 15));   /* 0xFFFF -> 65536 */
        return 0;
    }
    case 4: {
        UINT32 v[2];
        AudioGetVolume(s, &v[0], &v[1]);
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, v, sizeof(v))) ? 0 : (UINT64)(INT64)-1;
    }
    }
    return (UINT64)(INT64)-1;
}

void um_audio_syscalls_init(void)
{
    um_install(SYSCALL_NtNovaAudioOpen,  sys_open);
    um_install(SYSCALL_NtNovaAudioWrite, sys_write);
    um_install(SYSCALL_NtNovaAudioCtl,   sys_ctl);
}
