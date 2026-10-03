/*
 * NovaOS: the thread-pool "chores" behind std::async, as Microsoft's
 * <ppltaskscheduler.h> declares them (msvcp140.dll exports these).
 */
#pragma once
#include <crtdefs.h>

#pragma pack(push, 8)
namespace Concurrency {
namespace details {
struct _Threadpool_chore;
_CRTIMP2 int __cdecl _Schedule_chore(_Threadpool_chore *);
_CRTIMP2 int __cdecl _Reschedule_chore(const _Threadpool_chore *);
_CRTIMP2 void __cdecl _Release_chore(_Threadpool_chore *);

struct _Threadpool_chore {
    void *_M_work;
    void(__cdecl *_M_callback)(void *);
    void *_M_data;
    _Threadpool_chore(void(__cdecl *_Callback)(void *), void *_Data) : _M_work(nullptr), _M_callback(_Callback), _M_data(_Data) {}
    _Threadpool_chore() : _M_work(nullptr), _M_callback(nullptr), _M_data(nullptr) {}
    ~_Threadpool_chore() { _Release_chore(this); }
};
}
}
#pragma pack(pop)
