# after the restart (130-install-restart.py): linktest's two names are still one file
DOC = 'hard links kept across a restart (`linktest restarted`)'
TESTS = [
    Test('linktest restarted', 'linktest restarted', [r'linktest restarted: \d+ passed, 0 failed']),
]
