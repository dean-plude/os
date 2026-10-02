/*
 * wavein.c — waveform input.  NovaOS has no recording devices
 * (waveInGetNumDevs is 0 and waveInOpen fails), so no input handle exists
 * and every call on one reports it invalid.
 */
#include <windows.h>

#define MMAPI __declspec(dllexport)
typedef UINT MMRESULT;
#define MMSYSERR_INVALHANDLE 5

static MMRESULT no_input(void) { return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI waveInClose(HANDLE h) { (void)h; return no_input(); }
MMAPI MMRESULT WINAPI waveInStart(HANDLE h) { (void)h; return no_input(); }
MMAPI MMRESULT WINAPI waveInStop(HANDLE h) { (void)h; return no_input(); }
MMAPI MMRESULT WINAPI waveInReset(HANDLE h) { (void)h; return no_input(); }
MMAPI MMRESULT WINAPI waveInPrepareHeader(HANDLE h, void *hdr, UINT n) { (void)h; (void)hdr; (void)n; return no_input(); }
MMAPI MMRESULT WINAPI waveInUnprepareHeader(HANDLE h, void *hdr, UINT n) { (void)h; (void)hdr; (void)n; return no_input(); }
MMAPI MMRESULT WINAPI waveInAddBuffer(HANDLE h, void *hdr, UINT n) { (void)h; (void)hdr; (void)n; return no_input(); }
MMAPI MMRESULT WINAPI waveInGetPosition(HANDLE h, void *t, UINT n) { (void)h; (void)t; (void)n; return no_input(); }
MMAPI MMRESULT WINAPI waveInGetID(HANDLE h, UINT *id) { (void)h; if (id) *id = 0; return no_input(); }

/* the same texts as waveOut's */
MMAPI MMRESULT WINAPI waveOutGetErrorTextA(MMRESULT e, LPSTR out, UINT n);
MMAPI MMRESULT WINAPI waveOutGetErrorTextW(MMRESULT e, LPWSTR out, UINT n);
MMAPI MMRESULT WINAPI waveInGetErrorTextA(MMRESULT e, LPSTR out, UINT n) { return waveOutGetErrorTextA(e, out, n); }
MMAPI MMRESULT WINAPI waveInGetErrorTextW(MMRESULT e, LPWSTR out, UINT n) { return waveOutGetErrorTextW(e, out, n); }
