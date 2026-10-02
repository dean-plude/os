# The graphics boot: 7-Zip in C:\Programs\7-Zip and the Mesa and DXVK
# downloads in C:\Downloads (tools/ci/stage-graphics.sh), so the App
# Store's button installs without a network.  Everything after needs them.
TESTS = [
    Test('install Mesa 3D', 'store install Mesa 3D', store='Mesa 3D', timeout=1200),
    Test('install DXVK', 'store install DXVK', store='DXVK', timeout=600),
]
