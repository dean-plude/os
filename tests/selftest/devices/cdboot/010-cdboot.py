# nova.iso as a disc (QEMU's SATA DVD drive), the only thing to start
# from: the same ISO must still start as a disc now that it is also a USB
# stick image (a GPT partition over the El Torito image)
DOC = 'nova.iso as a disc in a SATA DVD drive: NovaOS starts live from it, the installation disc'
TESTS = [
    Test('disc live boot', 'cmd /c echo cdboot-ok', [r'cdboot-ok'],
         boot_expect=[r'\[SETUP\] Running from the installation disc']),
]
