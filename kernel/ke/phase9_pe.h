/*
 * phase9_pe.h — Embedded minimal PE32+ test binary for Phase 9
 *
 * A hand-assembled, import-free PE32+ console executable that proves
 * native ring-3 execution.  The entry point directly issues SYSCALL
 * instructions (no ntdll thunk needed) with Windows 10 1903 syscall numbers:
 *
 *   NtWriteFile        = 0x0008
 *   NtTerminateProcess = 0x002C
 *
 * Execution path:
 *   1. Align RSP to 16 bytes (IRETQ entry leaves RSP at stack_top - 8)
 *   2. Allocate 0x60-byte stack frame; zero the IoStatusBlock at [rsp]
 *   3. Set up NtWriteFile arguments per Windows x64 syscall ABI:
 *        [rsp+0x28] = IoStatusBlock*  (points to [rsp])
 *        [rsp+0x30] = Buffer*         (RIP-relative → "Hello from ring-3!\n")
 *        [rsp+0x38] = Length          (19 bytes)
 *        r10 = -11 (STD_OUTPUT_HANDLE), rdx/r8/r9 = 0, eax = 8
 *   4. SYSCALL  → kernel intercepts, routes stdout to serial/kprintf
 *   5. Set up NtTerminateProcess: r10 = -1, rdx = 0, eax = 0x2C
 *   6. SYSCALL  → kernel kills the thread
 *   7. Safety jmp $ (unreachable)
 *   8. Inline data: "Hello from ring-3!\n" (19 bytes)
 *
 * PE layout:
 *   0x000–0x03F  DOS header  (64 bytes, e_lfanew = 0x40)
 *   0x040–0x057  PE signature + IMAGE_FILE_HEADER
 *   0x058–0x147  IMAGE_OPTIONAL_HEADER64  (240 bytes)
 *   0x148–0x16F  IMAGE_SECTION_HEADER for .text
 *   0x170–0x1FF  Padding to file alignment (0x200)
 *   0x200–0x3FF  .text section (512 bytes, code at RVA 0x1000)
 *
 * ImageBase = 0x0000000140000000  (standard EXE preferred base)
 * SizeOfImage = 0x2000            (two pages: headers + code)
 */

#pragma once
#include "../include/types.h"

static const UINT8 g_phase9_pe[] = {

    /* ------------------------------------------------------------------ */
    /* DOS header  (64 bytes, 0x000–0x03F)                                */
    /* ------------------------------------------------------------------ */
    0x4D, 0x5A,                                           /* e_magic = MZ */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, /* e_lfanew=0x40 @0x3C */

    /* ------------------------------------------------------------------ */
    /* PE signature (4) + IMAGE_FILE_HEADER (20)  at 0x040               */
    /* ------------------------------------------------------------------ */
    0x50, 0x45, 0x00, 0x00,   /* Signature = "PE\0\0"                    */
    0x64, 0x86,               /* Machine = IMAGE_FILE_MACHINE_AMD64       */
    0x01, 0x00,               /* NumberOfSections = 1                     */
    0x00, 0x00, 0x00, 0x00,   /* TimeDateStamp                            */
    0x00, 0x00, 0x00, 0x00,   /* PointerToSymbolTable                     */
    0x00, 0x00, 0x00, 0x00,   /* NumberOfSymbols                          */
    0xF0, 0x00,               /* SizeOfOptionalHeader = 240 (0xF0)        */
    0x22, 0x00,               /* Characteristics: EXEC | LARGE_ADDR_AWARE */

    /* ------------------------------------------------------------------ */
    /* IMAGE_OPTIONAL_HEADER64  (240 bytes, 0x058–0x147)                  */
    /* ------------------------------------------------------------------ */
    0x0B, 0x02,               /* Magic = PE32+                            */
    0x00, 0x00,               /* MajorLinkerVersion, MinorLinkerVersion    */
    0x00, 0x02, 0x00, 0x00,   /* SizeOfCode = 0x200                       */
    0x00, 0x00, 0x00, 0x00,   /* SizeOfInitializedData                    */
    0x00, 0x00, 0x00, 0x00,   /* SizeOfUninitializedData                  */
    0x00, 0x10, 0x00, 0x00,   /* AddressOfEntryPoint = RVA 0x1000         */
    0x00, 0x10, 0x00, 0x00,   /* BaseOfCode = RVA 0x1000                  */
    /* ImageBase = 0x0000000140000000 (LE 8-byte) */
    0x00, 0x00, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x10, 0x00, 0x00,   /* SectionAlignment = 0x1000                */
    0x00, 0x02, 0x00, 0x00,   /* FileAlignment = 0x200                    */
    0x06, 0x00,               /* MajorOperatingSystemVersion = 6          */
    0x00, 0x00,               /* MinorOperatingSystemVersion              */
    0x00, 0x00,               /* MajorImageVersion                        */
    0x00, 0x00,               /* MinorImageVersion                        */
    0x06, 0x00,               /* MajorSubsystemVersion = 6 (Vista+)       */
    0x00, 0x00,               /* MinorSubsystemVersion                    */
    0x00, 0x00, 0x00, 0x00,   /* Win32VersionValue                        */
    0x00, 0x20, 0x00, 0x00,   /* SizeOfImage = 0x2000                     */
    0x00, 0x02, 0x00, 0x00,   /* SizeOfHeaders = 0x200                    */
    0x00, 0x00, 0x00, 0x00,   /* CheckSum                                 */
    0x03, 0x00,               /* Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI  */
    0x00, 0x00,               /* DllCharacteristics                       */
    /* SizeOfStackReserve = 0x100000 (1 MiB) */
    0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* SizeOfStackCommit = 0x1000 (4 KiB) */
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* SizeOfHeapReserve = 0x100000 (1 MiB) */
    0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* SizeOfHeapCommit = 0x1000 (4 KiB) */
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,   /* LoaderFlags                              */
    0x10, 0x00, 0x00, 0x00,   /* NumberOfRvaAndSizes = 16                 */
    /* DataDirectory[16] = 128 bytes, all zero (no exports/imports/relocs) */
    0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,   /* [0] Export, [1] Import       */
    0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,   /* [2] Resource, [3] Exception  */
    0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,   /* [4] Security, [5] BaseReloc  */
    0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,   /* [6] Debug, [7] Arch          */
    0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,   /* [8] GlobalPtr, [9] TLS       */
    0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,   /* [10] LoadConfig, [11] Bound  */
    0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,   /* [12] IAT, [13] DelayImport   */
    0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,   /* [14] CLR, [15] Reserved      */

    /* ------------------------------------------------------------------ */
    /* IMAGE_SECTION_HEADER for .text  (40 bytes, 0x148–0x16F)           */
    /* ------------------------------------------------------------------ */
    '.', 't', 'e', 'x', 't', 0, 0, 0,    /* Name[8]                      */
    0x7C, 0x00, 0x00, 0x00,              /* VirtualSize = 0x7C           */
    0x00, 0x10, 0x00, 0x00,              /* VirtualAddress = RVA 0x1000  */
    0x00, 0x02, 0x00, 0x00,              /* SizeOfRawData = 0x200        */
    0x00, 0x02, 0x00, 0x00,              /* PointerToRawData = 0x200     */
    0x00, 0x00, 0x00, 0x00,              /* PointerToRelocations         */
    0x00, 0x00, 0x00, 0x00,              /* PointerToLinenumbers         */
    0x00, 0x00,                          /* NumberOfRelocations          */
    0x00, 0x00,                          /* NumberOfLinenumbers          */
    0x20, 0x00, 0x00, 0x60,              /* Chars: CNT_CODE|MEM_EXE|READ */

    /* ------------------------------------------------------------------ */
    /* Padding  0x170–0x1FF  (144 bytes to reach FileAlignment boundary)  */
    /* ------------------------------------------------------------------ */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,

    /* ================================================================== */
    /* .text section  (512 bytes, file offset 0x200 = RVA 0x1000)        */
    /* ================================================================== */
    /*
     * Annotated disassembly (offsets relative to section start):
     *
     * +0x00  48 83 E4 F0      and  rsp, -16        ; 16-byte align
     * +0x04  48 83 EC 60      sub  rsp, 0x60        ; frame for args+io_status
     * +0x08  31 C0            xor  eax, eax
     * +0x0A  48 89 04 24      mov  [rsp], rax       ; io_status.Status = 0
     * +0x0E  48 89 44 24 08   mov  [rsp+8], rax     ; io_status.Information = 0
     * +0x13  48 8D 04 24      lea  rax, [rsp]       ; rax = &io_status
     * +0x17  48 89 44 24 28   mov  [rsp+0x28], rax  ; arg5 (IoStatusBlock*)
     * +0x1C  48 8D 05 46..    lea  rax, [rip+0x46]  ; rax = &hello_string
     *                                               ; RIP after = +0x23, str @+0x69
     *                                               ; 0x69 - 0x23 = 0x46 ✓
     * +0x23  48 89 44 24 30   mov  [rsp+0x30], rax  ; arg6 (Buffer*)
     * +0x28  B8 13 00 00 00   mov  eax, 19          ; 19 = length
     * +0x2D  48 89 44 24 38   mov  [rsp+0x38], rax  ; arg7 (Length)
     * +0x32  31 C0            xor  eax, eax
     * +0x34  48 89 44 24 40   mov  [rsp+0x40], rax  ; arg8 (ByteOffset*) = NULL
     * +0x39  48 89 44 24 48   mov  [rsp+0x48], rax  ; arg9 (Key*) = NULL
     * +0x3E  49 BA F5 FF..    mov  r10, -11         ; FileHandle = STD_OUTPUT
     * +0x48  31 D2            xor  edx, edx         ; Event = NULL
     * +0x4A  45 31 C0         xor  r8d, r8d         ; ApcRoutine = NULL
     * +0x4D  45 31 C9         xor  r9d, r9d         ; ApcContext = NULL
     * +0x50  B8 08 00 00 00   mov  eax, 8           ; NtWriteFile (Win10 1903)
     * +0x55  0F 05            syscall
     * +0x57  45 31 D2         xor  r10d, r10d
     * +0x5A  49 83 CA FF      or   r10, -1          ; ProcessHandle = -1
     * +0x5E  31 D2            xor  edx, edx         ; ExitStatus = 0
     * +0x60  B8 2C 00 00 00   mov  eax, 0x2C        ; NtTerminateProcess
     * +0x65  0F 05            syscall
     * +0x67  EB FE            jmp  $                ; safety trap (unreachable)
     * +0x69  "Hello from ring-3!\n"  (19 bytes)
     * +0x7C  <zeros to 0x200>
     */

    /* +0x00 */ 0x48, 0x83, 0xE4, 0xF0,
    /* +0x04 */ 0x48, 0x83, 0xEC, 0x60,
    /* +0x08 */ 0x31, 0xC0,
    /* +0x0A */ 0x48, 0x89, 0x04, 0x24,
    /* +0x0E */ 0x48, 0x89, 0x44, 0x24, 0x08,
    /* +0x13 */ 0x48, 0x8D, 0x04, 0x24,
    /* +0x17 */ 0x48, 0x89, 0x44, 0x24, 0x28,
    /* +0x1C */ 0x48, 0x8D, 0x05, 0x46, 0x00, 0x00, 0x00,
    /* +0x23 */ 0x48, 0x89, 0x44, 0x24, 0x30,
    /* +0x28 */ 0xB8, 0x13, 0x00, 0x00, 0x00,
    /* +0x2D */ 0x48, 0x89, 0x44, 0x24, 0x38,
    /* +0x32 */ 0x31, 0xC0,
    /* +0x34 */ 0x48, 0x89, 0x44, 0x24, 0x40,
    /* +0x39 */ 0x48, 0x89, 0x44, 0x24, 0x48,
    /* +0x3E */ 0x49, 0xBA, 0xF5, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* +0x48 */ 0x31, 0xD2,
    /* +0x4A */ 0x45, 0x31, 0xC0,
    /* +0x4D */ 0x45, 0x31, 0xC9,
    /* +0x50 */ 0xB8, 0x08, 0x00, 0x00, 0x00,
    /* +0x55 */ 0x0F, 0x05,
    /* +0x57 */ 0x45, 0x31, 0xD2,
    /* +0x5A */ 0x49, 0x83, 0xCA, 0xFF,
    /* +0x5E */ 0x31, 0xD2,
    /* +0x60 */ 0xB8, 0x2C, 0x00, 0x00, 0x00,
    /* +0x65 */ 0x0F, 0x05,
    /* +0x67 */ 0xEB, 0xFE,
    /* +0x69 "Hello from ring-3!\n" */
    'H','e','l','l','o',' ','f','r','o','m',' ','r','i','n','g','-','3','!','\n',

    /* Padding zeros: 0x200 - 0x7C = 0x184 bytes  (section must be 512 bytes) */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0x7C..0x8B  */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0x8C..0x9B  */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0x9C..0xAB  */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0xAC..0xBB  */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0xBC..0xCB  */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0xCC..0xDB  */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0xDC..0xEB  */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0xEC..0xFB  */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0x100..0x10F */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0x110..0x11F */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0x120..0x12F */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0x130..0x13F */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0x140..0x14F */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0x150..0x15F */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0x160..0x16F */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0x170..0x17F */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0x180..0x18F */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0x190..0x19F */
    0,0,0,0,                           /* 0x1A0..0x1A3 */
};

static const UINT32 g_phase9_pe_size = sizeof(g_phase9_pe);
