## OpenGL with no driver installed

Windows always has an OpenGL: without a display driver's, `opengl32.dll`
gives its own OpenGL 1.1 ("GDI Generic"), so a program can create a
context, read the version and fall back.  NovaOS's `opengl32.dll` loaded
Mesa 3D (the App Store's llvmpipe, or virgl on the host GPU) or nothing,
and with nothing every function returned 0, which Krita's Qt took for a
context and crashed on.  It now falls back to a small OpenGL 1.1 of its
own on GDI: a double-buffered RGBA pixel format, contexts made current per
thread, `glGetString` ("NovaOS", "GDI Generic", "1.1.0"), `glViewport`,
`glClearColor`, `glClear`, `glReadPixels` and `SwapBuffers` through
`SetDIBitsToDevice`.  New self-test `glgeneric` (64- and 32-bit, before
Mesa is installed).  Krita gets past start-up on it but still needs
OpenGL 2 for its Qt Quick widgets, so it still needs Mesa 3D.
