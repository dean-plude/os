/* sectest.exe — the kernel must refuse hostile system calls, not crash */
#include <stdio.h>
#include <string.h>
#include <winternl.h>

static int pass, fail;
#define EXPECT(what, got, want) do { unsigned long g_ = (unsigned long)(got); \
    if (g_ == (unsigned long)(want)) pass++; \
    else { fail++; printf("FAIL: %s -> 0x%08lx (expected 0x%08lx)\n", what, g_, (unsigned long)(want)); } } while (0)

#define STATUS_INVALID_SYSTEM_SERVICE 0xC000001C
#define KERNEL_PTR ((void *)0xFFFFFFFF80100000ULL)

static long long raw_syscall(long long num, long long a1, long long a2, long long a3, long long a4)
{
    register long long r10 __asm__("r10") = a1;
    register long long r8 __asm__("r8") = a3;
    register long long r9 __asm__("r9") = a4;
    long long ret;
    __asm__ volatile ("syscall" : "=a"(ret), "+r"(r10), "+r"(r8), "+r"(r9)
                      : "a"(num), "d"(a2) : "rcx", "r11", "memory");
    return ret;
}

int main(void)
{
    IO_STATUS_BLOCK io;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

    /* services the subsystem doesn't provide (legacy/internal ones) */
    EXPECT("NtQuerySystemInformation", raw_syscall(0x36, 5, 0, 0, 0), STATUS_INVALID_SYSTEM_SERVICE);
    EXPECT("kernel helper 0x1F7", raw_syscall(0x1F7, 0, 0, 0, 0), STATUS_INVALID_SYSTEM_SERVICE);
    EXPECT("syscall 0x1FF", raw_syscall(0x1FF, 1, 2, 3, 4), STATUS_INVALID_SYSTEM_SERVICE);
    EXPECT("syscall 0xFFFF", raw_syscall(0xFFFF, 0, 0, 0, 0), STATUS_INVALID_SYSTEM_SERVICE);

    /* kernel addresses as buffers */
    EXPECT("NtWriteFile(kernel buffer)", NtWriteFile(out, 0, 0, 0, &io, KERNEL_PTR, 16, 0, 0), STATUS_ACCESS_VIOLATION);
    EXPECT("NtWriteFile(unmapped buffer)", NtWriteFile(out, 0, 0, 0, &io, (void *)0x1000, 16, 0, 0), STATUS_ACCESS_VIOLATION);
    HANDLE f = CreateFileA("C:\\Welcome.txt", GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) f = CreateFileA("C:\\Documents\\Welcome.txt", GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    EXPECT("NtReadFile(kernel buffer)", NtReadFile(f, 0, 0, 0, &io, KERNEL_PTR, 16, 0, 0), STATUS_ACCESS_VIOLATION);
    EXPECT("NtReadFile(kernel IOSB)", NtReadFile(f, 0, 0, 0, (PIO_STATUS_BLOCK)KERNEL_PTR, (char[16]){0}, 16, 0, 0),
           STATUS_ACCESS_VIOLATION);
    CloseHandle(f);
    EXPECT("NtCreateFile(kernel attributes)", NtCreateFile(&f, GENERIC_READ, (POBJECT_ATTRIBUTES)KERNEL_PTR, &io, 0, 0, 0,
                                                           FILE_OPEN, 0, 0, 0), STATUS_ACCESS_VIOLATION);
    PVOID base = KERNEL_PTR;
    SIZE_T size = 4096;
    EXPECT("NtAllocateVirtualMemory(kernel address)", NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size,
                                                                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE),
           0xC0000018 /* CONFLICTING_ADDRESSES */);
    base = (PVOID)0x140000000ULL;                                   /* our own image */
    size = 0;
    EXPECT("NtFreeVirtualMemory(image)", NtFreeVirtualMemory(NtCurrentProcess(), &base, &size, MEM_RELEASE),
           0xC00000A0 /* MEMORY_NOT_ALLOCATED */);
    EXPECT("NtClose(bogus handle)", NtClose((HANDLE)0x12345678), 0xC0000008);
    EXPECT("NtClose(kernel pointer handle)", NtClose(KERNEL_PTR), 0xC0000008);

    printf("sectest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
