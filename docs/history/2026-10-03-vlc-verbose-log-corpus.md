## VLC's corpus test runs with its verbose log

The VLC corpus test (`tests/appcorpus/870-vlc.py`) now starts VLC with
`-vv`, so the sound check covers a verbose run too: VLC writes some 1,200
log lines while the clip starts, each one twice (to stderr and to
`OutputDebugString`), and all of them reach the kernel log, yet the
recording must still hold an unbroken 440 Hz tone.  Its screenshot now
waits for the colour bars a second time, after the sound has run: under
TCG the clip could loop in between, and VLC shows its cone while it starts
over.  Locally under TCG the run passes with a 26 s unbroken tone.

The Phase 19.6 notes blamed a change between #73 and #94 for VLC's audio
running late and dropping every buffer under `-vv`.  A/B runs under TCG
showed otherwise: the Phase 19.6 branch before it took in main (f37357d)
and the same branch with main up to #94 merged both played only a
30-40 ms blip of the 30 s clip, while current main plays it with a few
seconds of glitches at start and then an unbroken tone.  Nothing in that
range made it worse.  The scheduling work merged since (thread and process
priorities, kernel lock wake-ups) is the likely reason the audio thread
now keeps up; that range was not bisected.

The corpus's ffmpeg test, which makes the clip VLC plays, typed a
163-character command into a Terminal that takes at most 158, so ffmpeg
was handed `C:\Apps\i` as its output file and VLC had nothing to play.
The command drops `-c:a aac` (AAC is MP4's default sound codec anyway)
and fits.  Longer command lines in the Terminal remain a gap: Windows'
console takes up to 8,191 characters.
