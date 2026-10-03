# ffmpeg (Gyan's static GPL build, MinGW with winpthreads) (tools/appcorpus.py):
# makes an H.264 and AAC MP4 from its test sources (30 s of SMPTE colour
# bars at 10 frames a second with a 440 Hz tone, encoded with the fastest
# preset; VLC plays it later, 870-vlc.py), converts it to VP9 and Opus in
# WebM, and ffprobe reads the result back.  The MP4's audio is AAC by
# default (ffmpeg's choice for .mp4), and the command stays short: the
# Terminal took only the first 158 characters of a line, which cut the
# output file's name to C:\Apps\i (ffmpeg then exits with EINVAL, -22)
DOC = 'ffmpeg (an MP4 converted to WebM)'
F = rf'{A}\ffmpeg\bin'
APP = App('ffmpeg', '7.1.1',
          'https://github.com/GyanD/codexffmpeg/releases/download/7.1.1/ffmpeg-7.1.1-essentials_build.zip',
          'ffmpeg', [Test('ffmpeg -version', rf'{F}\ffmpeg.exe -hide_banner -version', [r'ffmpeg version 7\.1\.1']),
                     Test('ffmpeg make mp4', rf'{F}\ffmpeg.exe -v error -f lavfi -i smptebars=d=30:s=qvga:r=10 '
                          rf'-f lavfi -i sine=440:d=30 -c:v libx264 -preset ultrafast {A}\in.mp4', [],
                          timeout=900),
                     Test('ffmpeg mp4 to webm', rf'{F}\ffmpeg.exe -v error -i {A}\in.mp4 {A}\out.webm', [], timeout=900),
                     Test('ffprobe webm', rf'{F}\ffprobe.exe -v error -show_entries stream=codec_name -of csv=p=0 '
                          rf'{A}\out.webm', [r'vp9', r'opus'])],
          strip=1)
