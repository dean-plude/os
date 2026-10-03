/*
 * NovaOS: <vcruntime_new.h>'s operator new and delete declarations.
 */
#pragma once
#include <vcruntime.h>

#pragma pack(push, 8)
extern "C++" {
namespace std {
enum class align_val_t : size_t {};
struct nothrow_t { explicit nothrow_t() = default; };
extern nothrow_t const nothrow;
}

_NODISCARD _VCRT_ALLOCATOR void *__cdecl operator new(size_t _Size);
_NODISCARD _VCRT_ALLOCATOR void *__cdecl operator new(size_t _Size, std::nothrow_t const &) noexcept;
_NODISCARD _VCRT_ALLOCATOR void *__cdecl operator new[](size_t _Size);
_NODISCARD _VCRT_ALLOCATOR void *__cdecl operator new[](size_t _Size, std::nothrow_t const &) noexcept;
void __cdecl operator delete(void *_Block) noexcept;
void __cdecl operator delete(void *_Block, std::nothrow_t const &) noexcept;
void __cdecl operator delete[](void *_Block) noexcept;
void __cdecl operator delete[](void *_Block, std::nothrow_t const &) noexcept;
void __cdecl operator delete(void *_Block, size_t _Size) noexcept;
void __cdecl operator delete[](void *_Block, size_t _Size) noexcept;
_NODISCARD _VCRT_ALLOCATOR void *__cdecl operator new(size_t _Size, std::align_val_t _Al);
_NODISCARD _VCRT_ALLOCATOR void *__cdecl operator new(size_t _Size, std::align_val_t _Al, std::nothrow_t const &) noexcept;
_NODISCARD _VCRT_ALLOCATOR void *__cdecl operator new[](size_t _Size, std::align_val_t _Al);
_NODISCARD _VCRT_ALLOCATOR void *__cdecl operator new[](size_t _Size, std::align_val_t _Al, std::nothrow_t const &) noexcept;
void __cdecl operator delete(void *_Block, std::align_val_t _Al) noexcept;
void __cdecl operator delete(void *_Block, std::align_val_t _Al, std::nothrow_t const &) noexcept;
void __cdecl operator delete[](void *_Block, std::align_val_t _Al) noexcept;
void __cdecl operator delete[](void *_Block, std::align_val_t _Al, std::nothrow_t const &) noexcept;
void __cdecl operator delete(void *_Block, size_t _Size, std::align_val_t _Al) noexcept;
void __cdecl operator delete[](void *_Block, size_t _Size, std::align_val_t _Al) noexcept;

#ifndef __PLACEMENT_NEW_INLINE
#define __PLACEMENT_NEW_INLINE
_NODISCARD inline void *__cdecl operator new(size_t _Size, void *_Where) noexcept { (void)_Size; return _Where; }
inline void __cdecl operator delete(void *, void *) noexcept {}
#endif
#ifndef __PLACEMENT_VEC_NEW_INLINE
#define __PLACEMENT_VEC_NEW_INLINE
_NODISCARD inline void *__cdecl operator new[](size_t _Size, void *_Where) noexcept { (void)_Size; return _Where; }
inline void __cdecl operator delete[](void *, void *) noexcept {}
#endif
}
#pragma pack(pop)
