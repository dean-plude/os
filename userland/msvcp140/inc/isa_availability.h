/*
 * NovaOS: <isa_availability.h>, the instruction-set levels the VC
 * runtime's start-up code records in __isa_enabled (bit per level).
 * Written for NovaOS from the documented interface.
 */
#pragma once

enum ISA_AVAILABILITY {
    __ISA_AVAILABLE_X86 = 0,
    __ISA_AVAILABLE_SSE2 = 1,
    __ISA_AVAILABLE_SSE42 = 2,
    __ISA_AVAILABLE_AVX = 3,
    __ISA_AVAILABLE_ENFSTRG = 4,
    __ISA_AVAILABLE_AVX2 = 5,
    __ISA_AVAILABLE_AVX512 = 6,
};

#ifdef __cplusplus
extern "C" {
#endif
extern int __isa_available;
extern unsigned int __isa_enabled;
#ifdef __cplusplus
}
#endif
