# kmttest: the D3DKMT* calls (tools/selftest.py's core suite)
DOC = ('`kmttest` (the WDDM graphics kernel\'s D3DKMT* calls on NovaOS\'s Basic Render adapter, step 3 of '
       'docs/design/intel-gpu.md: adapters by LUID, devices, allocations counted as video memory and mapped by '
       '`D3DKMTLock2`, monitored fences signalled and waited on from the CPU, with an event or another thread)')
TESTS = [
    Test('kmttest', 'kmttest', [r'kmttest: \d+ passed, 0 failed'],
         boot_expect=[r'\[DXGK\] Basic Render adapter \(software, render only\)']),
]
