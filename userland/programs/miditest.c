/*
 * miditest — MIDI on NovaOS (winmm's synthesizer)
 *
 *   miditest [DIR]
 *
 * 1. midiOut: the device and its capabilities, a GM reset (midiOutLongMsg),
 *    then a flute (program 73) holding A4 (440 Hz) for a second.
 * 2. midiStream: E5 (659 Hz) for 192 ticks at 96 ticks per quarter note and
 *    120 beats a minute (one second), the last event asking for
 *    MOM_POSITIONCB; MOM_DONE must come and the stream's position must be
 *    about a second.
 * 3. MCI: a MIDI file written to DIR (default C:\) holding A5 (880 Hz) for
 *    a second, opened as a sequencer, its length asked for and played with
 *    "wait".
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef UINT MMRESULT;
typedef struct midihdr_tag {
    LPSTR lpData;
    DWORD dwBufferLength, dwBytesRecorded;
    DWORD_PTR dwUser;
    DWORD dwFlags;
    struct midihdr_tag *lpNext;
    DWORD_PTR reserved;
    DWORD dwOffset;
    DWORD_PTR dwReserved[8];
} MIDIHDR;
typedef struct { WORD wMid, wPid; UINT vDriverVersion; WCHAR szPname[32]; WORD wTechnology, wVoices, wNotes, wChannelMask; DWORD dwSupport; } MIDIOUTCAPSW;
typedef struct { UINT wType; union { DWORD ms, sample, cb, ticks; BYTE smpte[8]; } u; } MMTIME;

__declspec(dllimport) UINT WINAPI midiOutGetNumDevs(void);
__declspec(dllimport) MMRESULT WINAPI midiOutGetDevCapsW(UINT_PTR, MIDIOUTCAPSW *, UINT);
__declspec(dllimport) MMRESULT WINAPI midiOutOpen(HANDLE *, UINT, DWORD_PTR, DWORD_PTR, DWORD);
__declspec(dllimport) MMRESULT WINAPI midiOutClose(HANDLE);
__declspec(dllimport) MMRESULT WINAPI midiOutShortMsg(HANDLE, DWORD);
__declspec(dllimport) MMRESULT WINAPI midiOutLongMsg(HANDLE, MIDIHDR *, UINT);
__declspec(dllimport) MMRESULT WINAPI midiOutPrepareHeader(HANDLE, MIDIHDR *, UINT);
__declspec(dllimport) MMRESULT WINAPI midiOutUnprepareHeader(HANDLE, MIDIHDR *, UINT);
__declspec(dllimport) MMRESULT WINAPI midiStreamOpen(HANDLE *, UINT *, DWORD, DWORD_PTR, DWORD_PTR, DWORD);
__declspec(dllimport) MMRESULT WINAPI midiStreamClose(HANDLE);
__declspec(dllimport) MMRESULT WINAPI midiStreamOut(HANDLE, MIDIHDR *, UINT);
__declspec(dllimport) MMRESULT WINAPI midiStreamRestart(HANDLE);
__declspec(dllimport) MMRESULT WINAPI midiStreamStop(HANDLE);
__declspec(dllimport) MMRESULT WINAPI midiStreamPosition(HANDLE, MMTIME *, UINT);
__declspec(dllimport) MMRESULT WINAPI midiStreamProperty(HANDLE, BYTE *, DWORD);
__declspec(dllimport) DWORD WINAPI mciSendStringA(LPCSTR, LPSTR, UINT, HWND);
__declspec(dllimport) BOOL WINAPI mciGetErrorStringA(DWORD, LPSTR, UINT);

#define CALLBACK_FUNCTION 0x00030000
#define MOM_DONE          0x3C9
#define MOM_POSITIONCB    0x3CA
#define MEVT_F_CALLBACK   0x40000000

static int test_out(void)
{
    UINT n = midiOutGetNumDevs();
    MIDIOUTCAPSW caps;
    MMRESULT r = midiOutGetDevCapsW(0, &caps, sizeof(caps));
    printf("midiOut: %u device(s); \"%ls\" technology %u, support %lx (%u)\n", n, caps.szPname, caps.wTechnology,
           caps.dwSupport, r);
    if (n != 1 || r) { printf("FAIL no MIDI output device\n"); return 1; }
    HANDLE h;
    r = midiOutOpen(&h, 0, 0, 0, 0);
    if (r) { printf("FAIL midiOutOpen: %u\n", r); return 1; }
    BYTE gm_on[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
    MIDIHDR hdr = { (LPSTR)gm_on, sizeof(gm_on), sizeof(gm_on) };
    midiOutPrepareHeader(h, &hdr, sizeof(hdr));
    r = midiOutLongMsg(h, &hdr, sizeof(hdr));
    midiOutUnprepareHeader(h, &hdr, sizeof(hdr));
    if (r || !(hdr.dwFlags & 1)) { printf("FAIL midiOutLongMsg: %u\n", r); return 1; }
    midiOutShortMsg(h, 0xC0 | 73 << 8);                     /* flute */
    midiOutShortMsg(h, 0x90 | 69 << 8 | 110 << 16);         /* A4 on */
    Sleep(1000);
    midiOutShortMsg(h, 0x80 | 69 << 8);
    Sleep(300);
    r = midiOutClose(h);
    printf("midiOut: A4 for 1000 ms; close %u\n", r);
    return r != 0;
}

static volatile LONG g_done, g_poscb;
static HANDLE g_ev;
static void CALLBACK stream_cb(HANDLE h, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2)
{
    (void)h; (void)inst; (void)p1; (void)p2;
    if (msg == MOM_DONE) { InterlockedIncrement(&g_done); SetEvent(g_ev); }
    if (msg == MOM_POSITIONCB) InterlockedIncrement(&g_poscb);
}

static int test_stream(void)
{
    HANDLE h;
    UINT dev = 0;
    g_ev = CreateEventW(0, FALSE, FALSE, 0);
    MMRESULT r = midiStreamOpen(&h, &dev, 1, (DWORD_PTR)stream_cb, 0, CALLBACK_FUNCTION);
    if (r) { printf("FAIL midiStreamOpen: %u\n", r); return 1; }
    DWORD div[2] = { 8, 96 }, tempo[2] = { 8, 500000 };
    midiStreamProperty(h, (BYTE *)div, 0x80000000 | 1);
    midiStreamProperty(h, (BYTE *)tempo, 0x80000000 | 2);
    DWORD ev[] = {
        0, 0, 0xC0 | 73 << 8,                               /* flute */
        0, 0, 0x90 | 76 << 8 | 110 << 16,                   /* E5 on */
        192, 0, MEVT_F_CALLBACK | 0x80 | 76 << 8,           /* off two beats later */
    };
    MIDIHDR hdr = { (LPSTR)ev, sizeof(ev), sizeof(ev) };
    midiOutPrepareHeader(h, &hdr, sizeof(hdr));
    r = midiStreamOut(h, &hdr, sizeof(hdr));
    if (r) { printf("FAIL midiStreamOut: %u\n", r); return 1; }
    DWORD t0 = GetTickCount();
    midiStreamRestart(h);
    DWORD w = WaitForSingleObject(g_ev, 4000);
    DWORD elapsed = GetTickCount() - t0;
    Sleep(100);
    MMTIME t = { 0x01 /* TIME_MS */ };
    midiStreamPosition(h, &t, sizeof(t));
    MMTIME tk = { 0x20 /* TIME_TICKS */ };
    midiStreamPosition(h, &tk, sizeof(tk));
    midiStreamStop(h);
    midiOutUnprepareHeader(h, &hdr, sizeof(hdr));
    r = midiStreamClose(h);
    printf("midiStream: done %ld, position callbacks %ld in %lu ms; position %lu ms, %lu ticks; close %u\n", g_done,
           g_poscb, elapsed, t.u.ms, tk.u.ticks, r);
    int bad = 0;
    if (w != WAIT_OBJECT_0 || g_done != 1 || g_poscb != 1) { printf("FAIL stream callbacks\n"); bad = 1; }
    if (elapsed < 900 || elapsed > 1500) { printf("FAIL the stream took %lu ms\n", elapsed); bad = 1; }
    if (t.u.ms < 850 || t.u.ms > 1300 || tk.u.ticks < 160 || tk.u.ticks > 260) { printf("FAIL stream position\n"); bad = 1; }
    return bad || r;
}

/* A format 0 MIDI file: tempo 120, flute, A5 for one second (two beats of 96 ticks) */
static BOOL write_mid(const char *path)
{
    static const BYTE mid[] = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,
        'M', 'T', 'r', 'k', 0, 0, 0, 22,
        0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,           /* tempo 500000 */
        0x00, 0xC0, 73,                                     /* flute */
        0x00, 0x90, 81, 110,                                /* A5 on */
        0x81, 0x40, 0x80, 81, 0,                            /* off after 192 ticks */
        0x00, 0xFF, 0x2F, 0x00,                             /* end of track */
    };
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    DWORD n;
    BOOL ok = f != INVALID_HANDLE_VALUE && WriteFile(f, mid, sizeof(mid), &n, 0) && n == sizeof(mid);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return ok;
}

static DWORD mci(const char *cmd, char *ret, UINT n)
{
    DWORD e = mciSendStringA(cmd, ret, n, 0);
    if (e) {
        char t[128];
        mciGetErrorStringA(e, t, sizeof(t));
        printf("FAIL \"%s\": %lu %s\n", cmd, e, t);
    }
    return e;
}

static int test_mci(const char *dir)
{
    char path[MAX_PATH], cmd[MAX_PATH + 64], len[32] = "", mode[32] = "";
    snprintf(path, sizeof(path), "%s%stest.mid", dir, dir[strlen(dir) - 1] == '\\' ? "" : "\\");
    if (!write_mid(path)) { printf("FAIL cannot write %s\n", path); return 1; }
    snprintf(cmd, sizeof(cmd), "open \"%s\" type sequencer alias tune", path);
    if (mci(cmd, 0, 0)) return 1;
    if (mci("status tune length", len, sizeof(len))) return 1;
    DWORD t0 = GetTickCount();
    if (mci("play tune wait", 0, 0)) return 1;
    DWORD elapsed = GetTickCount() - t0;
    mci("status tune mode", mode, sizeof(mode));
    DWORD e = mci("close tune", 0, 0);
    printf("MCI: length %s ms, played in %lu ms, then %s\n", len, elapsed, mode);
    int bad = e != 0;
    if (atoi(len) < 950 || atoi(len) > 1050) { printf("FAIL length\n"); bad = 1; }
    if (elapsed < 900 || elapsed > 1500) { printf("FAIL playing took %lu ms\n", elapsed); bad = 1; }
    if (strcmp(mode, "stopped")) { printf("FAIL mode %s\n", mode); bad = 1; }
    return bad;
}

int main(int argc, char **argv)
{
    int bad = test_out();
    Sleep(300);
    if (!bad) bad += test_stream();
    Sleep(300);
    if (!bad) bad += test_mci(argc > 1 ? argv[1] : "C:\\");
    printf("%s\n", bad ? "FAILED" : "all MIDI tests passed");
    return bad != 0;
}
