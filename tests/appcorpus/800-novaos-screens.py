# NovaOS's own screens (Phase 17.6): dir on C: and on the NTFS drive D:,
# then File Explorer's This PC, whose screenshot must match
# tests/reference/this-pc.png.  No download; not in README's list (no DOC).
# File Explorer opens at the first free cascade step, below the Terminal,
# whichever of NovaOS's own windows earlier programs opened and closed.
APP = App('NovaOS', 'screens', None, '',
          [Test('dir C:', 'dir C:\\', [r'Volume in drive C is', r'Dir\(s\)\s+[\d,]+ bytes free']),
           Test('dir D:', 'dir D:\\', [rf'Volume in drive D is {DRIVE_LABEL}', r'Dir\(s\)\s+[\d,]+ bytes free']),
           Test('This PC', 'start explorer', [])],
          unpack=None)
