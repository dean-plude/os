# the display mode chosen with CDS_UPDATEREGISTRY: the boot after the
# restart (130) must come up in it (132)
DOC = '`disptest 1024 768` (saves the mode the restart must keep)'
TESTS = [
    Test('save display mode', 'disptest 1024 768', [r'ChangeDisplaySettings: 0', r'current 1024 x 768'], settle=5),
]
