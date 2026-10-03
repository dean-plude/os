# The App Store's "Venus" (tools/build_venus.py's venus.7z in C:\Downloads):
# Mesa's Vulkan driver for the 3D virtio-gpu, which the Vulkan loader then
# prefers to Mesa 3D's lavapipe
TESTS = [
    Test('install Venus', 'store install Venus', store='Venus', timeout=300),
]
