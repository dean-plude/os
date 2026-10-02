# after the restart (130): NovaOS came up in the mode 128 saved
DOC = '`disptest saved 1024 768` (the restart kept the saved display mode)'
TESTS = [
    Test('mode after restart', 'disptest saved 1024 768', [r'\d+ passed, 0 failed'],
         boot_expect=[r'\[DISPLAY\] Restored the saved mode 1024x768']),
]
