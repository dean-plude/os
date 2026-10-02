# ffmpeg (Gyan's static GPL build, MinGW with winpthreads) (tools/appcorpus.py):
# makes an H.264 and AAC MP4 from its test sources, converts it to VP9 and
# Opus in WebM, and ffprobe reads the result back
DOC = 'ffmpeg (an MP4 converted to WebM)'
F = rf'{A}\ffmpeg\bin'
APP = App('ffmpeg', '7.1.1',
          'https://github.com/GyanD/codexffmpeg/releases/download/7.1.1/ffmpeg-7.1.1-essentials_build.zip',
          'ffmpeg', [Test('ffmpeg -version', rf'{F}\ffmpeg.exe -hide_banner -version', [r'ffmpeg version 7\.1\.1']),
                     Test('ffmpeg make mp4', rf'{F}\ffmpeg.exe -v error -f lavfi -i testsrc=d=1:s=320x240 -f lavfi '
                          rf'-i sine=d=1 -c:v libx264 -c:a aac {A}\in.mp4', [], timeout=600),
                     Test('ffmpeg mp4 to webm', rf'{F}\ffmpeg.exe -v error -i {A}\in.mp4 {A}\out.webm', [], timeout=900),
                     Test('ffprobe webm', rf'{F}\ffprobe.exe -v error -show_entries stream=codec_name -of csv=p=0 '
                          rf'{A}\out.webm', [r'vp9', r'opus'])],
          strip=1)
