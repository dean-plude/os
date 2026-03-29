/*
 * types.h — fundamental types for the NovaOS kernel
 *
 * These types are used everywhere in the kernel. We define our own so we
 * don't depend on a hosted C library. The kernel is compiled freestanding
 * (-ffreestanding -nostdlib), so <stdint.h> and <stdbool.h> are the only
 * compiler-provided headers we use (they're provided by GCC/Clang even in
 * freestanding mode).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Convenience aliases matching NT driver conventions */
typedef uint8_t  UCHAR;
typedef uint16_t USHORT;
typedef uint32_t ULONG;
typedef uint64_t ULONG64;
typedef uint64_t ULONGLONG;
typedef int8_t   CHAR;
typedef int8_t   INT8;
typedef int16_t  SHORT;
typedef int32_t  LONG;
typedef int64_t  LONGLONG;
typedef void *   PVOID;

/* NT-style status code (NTSTATUS — 32-bit) */
typedef int32_t  NTSTATUS;

/* NTSTATUS severity/facility macros */
#define NT_SUCCESS(s)      ((NTSTATUS)(s) >= 0)
#define NT_INFORMATION(s)  (((ULONG)(s) >> 30) == 1)
#define NT_WARNING(s)      (((ULONG)(s) >> 30) == 2)
#define NT_ERROR(s)        (((ULONG)(s) >> 30) == 3)

/* Common NTSTATUS codes */
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000)
#define STATUS_PENDING                   ((NTSTATUS)0x00000103)
#define STATUS_NO_MORE_ENTRIES           ((NTSTATUS)0x8000001A)
#define STATUS_BUFFER_OVERFLOW           ((NTSTATUS)0x80000005)
#define STATUS_UNSUCCESSFUL              ((NTSTATUS)0xC0000001)
#define STATUS_NOT_IMPLEMENTED           ((NTSTATUS)0xC0000002)
#define STATUS_INVALID_INFO_CLASS        ((NTSTATUS)0xC0000003)
#define STATUS_INVALID_HANDLE            ((NTSTATUS)0xC0000008)
#define STATUS_END_OF_FILE               ((NTSTATUS)0xC0000011)
#define STATUS_CONFLICTING_ADDRESSES     ((NTSTATUS)0xC0000018)
#define STATUS_NOT_MAPPED_VIEW           ((NTSTATUS)0xC0000019)
#define STATUS_INVALID_PARAMETER         ((NTSTATUS)0xC000000D)
#define STATUS_INVALID_DEVICE_REQUEST    ((NTSTATUS)0xC0000010)
#define STATUS_NO_MEMORY                 ((NTSTATUS)0xC0000017)
#define STATUS_INVALID_SYSTEM_SERVICE    ((NTSTATUS)0xC000001C)
#define STATUS_ACCESS_DENIED             ((NTSTATUS)0xC0000022)
#define STATUS_BUFFER_TOO_SMALL          ((NTSTATUS)0xC0000023)
#define STATUS_OBJECT_NAME_NOT_FOUND     ((NTSTATUS)0xC0000034)
#define STATUS_ALREADY_EXISTS            ((NTSTATUS)0xC0000035)
#define STATUS_OBJECT_PATH_NOT_FOUND     ((NTSTATUS)0xC000003A)
#define STATUS_IMAGE_MACHINE_TYPE_MISMATCH ((NTSTATUS)0xC0000058)
#define STATUS_INVALID_IMAGE_FORMAT      ((NTSTATUS)0xC000007B)
#define STATUS_MEMORY_NOT_ALLOCATED      ((NTSTATUS)0xC00000A0)
#define STATUS_INSUFFICIENT_RESOURCES    ((NTSTATUS)0xC000009A)
#define STATUS_PRIVILEGE_NOT_HELD        ((NTSTATUS)0xC0000061)
#define STATUS_CANNOT_DELETE             ((NTSTATUS)0xC0000121)
#define STATUS_NOT_FOUND                 ((NTSTATUS)0xC0000225)
#define STATUS_OBJECT_TYPE_MISMATCH      ((NTSTATUS)0xC0000024)
#define STATUS_OBJECT_PATH_INVALID       ((NTSTATUS)0xC0000039)
#define STATUS_OBJECT_NAME_COLLISION     ((NTSTATUS)0xC0000035)
#define STATUS_OBJECT_NAME_EXISTS        ((NTSTATUS)0x40000000)

/* NT type aliases — uppercase UINT */
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef int32_t   INT32;
typedef int64_t   INT64;
typedef uintptr_t ULONG_PTR;

/* Physical / virtual address types */
typedef uintptr_t   VADDR;    /* Virtual address */
typedef uintptr_t   PADDR;    /* Physical address */
typedef uintptr_t   KADDR;    /* Kernel virtual address */

/* Page-related constants */
#define PAGE_SIZE       4096UL
#define PAGE_SHIFT      12
#define PAGE_MASK       (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(x)   (((uintptr_t)(x) + PAGE_SIZE - 1) & PAGE_MASK)
#define PAGE_ALIGN_DOWN(x) ((uintptr_t)(x) & PAGE_MASK)
#define PAGE_OF(x)      ((uintptr_t)(x) >> PAGE_SHIFT)

/* 2MB huge page */
#define HUGE_PAGE_SIZE  (2UL * 1024 * 1024)
#define HUGE_PAGE_SHIFT 21
#define HUGE_PAGE_MASK  (~(HUGE_PAGE_SIZE - 1))

/* Kernel virtual address space layout
 * Must match the linker script and bootloader paging setup. */
#define KERNEL_VIRT_BASE    UINT64_C(0xFFFFFFFF80000000)  /* -2 GiB */
#define PHYSMAP_BASE        UINT64_C(0xFFFF800000000000)  /* Direct phys map */
#define PHYSMAP_SIZE        (64ULL * 1024 * 1024 * 1024)  /* 64 GiB */

/* Convert physical ↔ kernel virtual (physmap) */
#define PHYS_TO_VIRT(p)    ((void *)((uintptr_t)(p) + PHYSMAP_BASE))
#define VIRT_TO_PHYS(v)    ((uintptr_t)(v) - PHYSMAP_BASE)

/* Alignment helpers */
#define ALIGN_UP(x, a)   (((uintptr_t)(x) + (a) - 1) & ~((uintptr_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((uintptr_t)(x) & ~((uintptr_t)(a) - 1))
#define IS_ALIGNED(x, a) (!((uintptr_t)(x) & ((uintptr_t)(a) - 1)))

/* Attribute macros */
#define __packed    __attribute__((packed))
#define __noreturn  __attribute__((noreturn))
#define __unused    __attribute__((unused))
#define __used      __attribute__((used))
#define __section(s) __attribute__((section(s)))
#define __aligned(a) __attribute__((aligned(a)))
#define __naked     __attribute__((naked))
#define __noinline  __attribute__((noinline))
#define __always_inline __attribute__((always_inline)) inline

/* Min/max */
#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define MAX(a, b)  ((a) > (b) ? (a) : (b))

/* Array length */
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Bit operations */
#define BIT(n)          (1UL << (n))
#define BIT64(n)        (UINT64_C(1) << (n))
#define BITMASK(hi, lo) (((UINT64_C(2) << (hi)) - 1) & ~((UINT64_C(1) << (lo)) - 1))
