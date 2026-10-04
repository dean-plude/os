# The update channel's signature (kernel/fs/update.c): the test channels
# tools/selftest.py serves (update_boot) are signed with the self-tests'
# key, TEST-ONLY-signing-key.txt here, whose public half QEMU hands to
# NovaOS (fw_cfg opt/novaos/update-key).  Before any update is installed,
# three channels for the next version are refused: one with no signature,
# one signed with another key, and one changed after it was signed.  The
# signed channels of 010-update.py are then accepted.

CHANNEL = 'http://10.0.2.2:18090/{}/novaos-update.txt'
TRUSTING = r"\[UPDATE\] Also trusting the update signing key QEMU's host gave \(8cafb488b4fdb69e"

TESTS = [
    Test('unsigned channel', f'update channel {CHANNEL.format("unsigned")}',
         [r'Update channel: http://10\.0\.2\.2:18090/unsigned/'], builtin=True),
    Test('unsigned refused', 'update', [r'The update channel is not signed, and this NovaOS installs only signed updates'],
         builtin=True, timeout=120, boot_expect=[TRUSTING]),
    Test('other key channel', f'update channel {CHANNEL.format("otherkey")}',
         [r'Update channel: http://10\.0\.2\.2:18090/otherkey/'], builtin=True),
    Test('other key refused', 'update', [r'The update channel is signed with a key this NovaOS does not trust'],
         builtin=True, timeout=120),
    Test('changed channel', f'update channel {CHANNEL.format("changed")}',
         [r'Update channel: http://10\.0\.2\.2:18090/changed/'], builtin=True),
    Test('changed refused', 'update', [r"The update channel's signature is not valid: the file was changed after it was signed"],
         builtin=True, timeout=120),
]
