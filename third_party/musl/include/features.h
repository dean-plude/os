/* NovaOS shim for musl's <features.h>: only what src/math needs */
#pragma once
#define _Noreturn __attribute__((__noreturn__))
#define hidden
#define weak_alias(old, new) \
    extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))
