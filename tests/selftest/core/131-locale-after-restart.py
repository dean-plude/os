# after the restart (130): the user locale 129 chose is still ja-JP; then
# back to en-US
DOC = '`nlstest after-restart ja-JP` (the restart kept the user locale)'
TESTS = [
    Test('locale after restart', 'nlstest after-restart ja-JP', [r'user locale: ja-JP', r'nlstest after-restart: \d+ passed, 0 failed'],
         settle=3),
]
