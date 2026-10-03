# Windows Installer: transforms, patches, rollback of a failed install, and
# an automatic service started at boot (services.exe).  The packages are
# tools/msitest/mkpkg.py's, in C:\Tests\Msi.  Later tests run in the new boot.
DOC = ('Windows Installer transforms, patches and rollback, and an installed service started at the next boot '
       '(`msitest transform`, `patch`, `rollback`, `service`, `shutdown /r`, `msitest service-boot`)')
TESTS = [
    Test('msi transform', 'msitest transform', [r'msitest transform: \d+ passed, 0 failed']),
    Test('msi patch', 'msitest patch', [r'msitest patch: \d+ passed, 0 failed']),
    Test('msi rollback', 'msitest rollback', [r'msitest rollback: \d+ passed, 0 failed']),
    Test('msi service', 'msitest service', [r'msitest service: \d+ passed, 0 failed']),
    Test('services at boot', 'shutdown /r', [], reboot=True),
    # (services.exe may not even have been launched yet when the Terminal is
    # back on a slow runner, so its log lines, the launch included, are
    # checked after msitest has waited for the service)
    Test('msi service at boot', 'msitest service-boot', [r'msitest service-boot: \d+ passed, 0 failed'],
         boot_expect=[r'command line: services /autostart', r'\[SVC\] NovaMsiTestSvc: started',
                      r'\[SVC\] Started \d+ of \d+ automatic services']),
]
