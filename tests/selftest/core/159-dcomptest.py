# dcomptest: Direct3D without a GPU, as Chromium's software compositor draws (WebView2's, Steam's and Galaxy's
# browsers): dxgi.dll's one adapter, the Microsoft Basic Render Driver; a WARP device; a DirectComposition
# target and visual showing a swap chain made for composition; frames mapped, copied and presented with dirty
# rectangles; a swap chain on a window; and the GPU process's compositing window parented in the browser's
# by another process.  The software device is 64-bit; the 32-bit run checks the adapter and the parenting.
DOC = ('`dcomptest` (Direct3D without a GPU as Chromium\'s software compositor uses it: the Basic Render Driver '
       'adapter, a WARP device, DirectComposition showing a swap chain, dirty-rectangle presents, a GPU process\'s '
       'window parented in another process\'s; 64- and 32-bit)')
TESTS = [
    Test('dcomptest x64', 'dcomptest', [r'dcomptest: \d+ passed, 0 failed \(64-bit\)'], timeout=240),
    Test('dcomptest x86', r'C:\Programs\x86\dcomptest.exe', [r'dcomptest: \d+ passed, 0 failed \(32-bit\)'], timeout=240),
]
