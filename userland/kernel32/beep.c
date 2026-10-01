/*
 * beep.c — Beep: a sine tone on the sound card
 *
 * Windows plays Beep through the default audio device; so does NovaOS,
 * on a mixer stream of its own (the kernel half of winmm's streams).
 * Without a sound card it waits out the duration silently.
 */

#define NOVA_BUILD_KERNEL32
#include <winternl.h>
#include "k32.h"

/* one cycle of a sine, 256 steps, about -8.7 dBFS */
static const short g_sine[256] = {
    0, 294, 589, 883, 1176, 1469, 1761, 2052, 2341, 2629, 2916, 3201, 3483, 3764, 4043, 4319,
    4592, 4863, 5131, 5395, 5657, 5915, 6169, 6420, 6667, 6910, 7148, 7383, 7613, 7838, 8059, 8274,
    8485, 8691, 8891, 9087, 9276, 9460, 9638, 9811, 9978, 10138, 10293, 10441, 10583, 10719, 10848, 10971,
    11087, 11196, 11299, 11394, 11483, 11565, 11640, 11708, 11769, 11823, 11870, 11910, 11942, 11967, 11986, 11996,
    12000, 11996, 11986, 11967, 11942, 11910, 11870, 11823, 11769, 11708, 11640, 11565, 11483, 11394, 11299, 11196,
    11087, 10971, 10848, 10719, 10583, 10441, 10293, 10138, 9978, 9811, 9638, 9460, 9276, 9087, 8891, 8691,
    8485, 8274, 8059, 7838, 7613, 7383, 7148, 6910, 6667, 6420, 6169, 5915, 5657, 5395, 5131, 4863,
    4592, 4319, 4043, 3764, 3483, 3201, 2916, 2629, 2341, 2052, 1761, 1469, 1176, 883, 589, 294,
    0, -294, -589, -883, -1176, -1469, -1761, -2052, -2341, -2629, -2916, -3201, -3483, -3764, -4043, -4319,
    -4592, -4863, -5131, -5395, -5657, -5915, -6169, -6420, -6667, -6910, -7148, -7383, -7613, -7838, -8059, -8274,
    -8485, -8691, -8891, -9087, -9276, -9460, -9638, -9811, -9978, -10138, -10293, -10441, -10583, -10719, -10848, -10971,
    -11087, -11196, -11299, -11394, -11483, -11565, -11640, -11708, -11769, -11823, -11870, -11910, -11942, -11967, -11986, -11996,
    -12000, -11996, -11986, -11967, -11942, -11910, -11870, -11823, -11769, -11708, -11640, -11565, -11483, -11394, -11299, -11196,
    -11087, -10971, -10848, -10719, -10583, -10441, -10293, -10138, -9978, -9811, -9638, -9460, -9276, -9087, -8891, -8691,
    -8485, -8274, -8059, -7838, -7613, -7383, -7148, -6910, -6667, -6420, -6169, -5915, -5657, -5395, -5131, -4863,
    -4592, -4319, -4043, -3764, -3483, -3201, -2916, -2629, -2341, -2052, -1761, -1469, -1176, -883, -589, -294,
};

/* the kernel's AudioStatus (kernel/drivers/audio.h) */
typedef struct {
    ULONGLONG written, consumed, played;
    UINT32 queued, capacity, running, latency;
} StreamStatus;

WINBASEAPI BOOL WINAPI Beep(DWORD freq, DWORD ms)
{
    if (freq < 37 || freq > 32767) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    INT_PTR s = NtNovaAudioOpen(48000 / 4);
    if (!s) {
        Sleep(ms);
        return TRUE;
    }
    NtNovaAudioCtl(s, 1, 1, 0);
    ULONGLONG total = (ULONGLONG)ms * 48 + 1, made = 0;
    UINT32 phase = 0, step = (UINT32)(((ULONGLONG)freq << 32) / 48000);
    short buf[480 * 2];
    while (made < total) {
        UINT32 n = total - made < 480 ? (UINT32)(total - made) : 480;
        for (UINT32 i = 0; i < n; i++) {
            UINT32 fade = (UINT32)(made + i < 240 ? made + i : total - made - i < 240 ? total - made - i : 240);
            short v = (short)(g_sine[phase >> 24] * (int)fade / 240);        /* 5 ms ramps: no clicks */
            buf[i * 2] = buf[i * 2 + 1] = v;
            phase += step;
        }
        UINT32 done = 0;
        while (done < n) {
            LONG_PTR got = NtNovaAudioWrite(s, buf + done * 2, n - done);
            if (got < 0) { NtClose((HANDLE)s); return FALSE; }
            done += (UINT32)got;
            if (done < n) Sleep(10);
        }
        made += n;
    }
    StreamStatus st;
    while (NtNovaAudioCtl(s, 0, 0, &st) == 0 && st.played < st.written) Sleep(5);
    NtClose((HANDLE)s);
    return TRUE;
}
