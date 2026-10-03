/*
 * NovaOS: the part of Microsoft's Parallel Patterns Library (<ppltasks.h>)
 * that msvcp140.dll implements: the out-of-line helpers MSVC-built
 * programs call from their own inlined copies of the task library.  The
 * names, member access and layouts are the ones those programs import.
 * task<void> and create_task are a small stand-in, enough for std::async
 * in NovaOS's own C++ programs: each task is a thread of its own.
 */
#pragma once
#include <crtdefs.h>
#include <exception>
#include <functional>
#include <ppltaskscheduler.h>
#include <process.h>

#pragma pack(push, 8)
namespace Concurrency {
namespace details {
namespace platform {
_CRTIMP2 long __cdecl GetCurrentThreadId();
_CRTIMP2 size_t __cdecl CaptureCallstack(void **, size_t, size_t);
_CRTIMP2 unsigned int __cdecl GetNextAsyncId();
}

[[noreturn]] _CRTIMP2 void __cdecl _ReportUnobservedException();

struct _Task_impl_base {
    _CRTIMP2 static bool __cdecl _IsNonBlockingThread();
};

class _TaskEventLogger {
    _Task_impl_base *_M_task;
    bool _M_scheduled;
    bool _M_taskPostEventStarted;
public:
    _CRTIMP2 void __thiscall _LogScheduleTask(bool _IsContinuation);
    _CRTIMP2 void __thiscall _LogCancelTask();
    _CRTIMP2 void __thiscall _LogTaskCompleted();
    _CRTIMP2 void __thiscall _LogTaskExecutionCompleted();
    _CRTIMP2 void __thiscall _LogWorkItemStarted();
    _CRTIMP2 void __thiscall _LogWorkItemCompleted();
};

class _ContextCallback {
    typedef std::function<void()> _CallbackFunction;
public:
    explicit _ContextCallback(bool _DeferCapture = false) {
        if (_DeferCapture) _M_context._M_captureMethod = _S_captureDeferred;
        else { _M_context._M_pContextCallback = nullptr; _Capture(); }
    }
    ~_ContextCallback() { _Reset(); }
    _CRTIMP2 void __thiscall _CallInContext(_CallbackFunction _Func, bool _IgnoreDisconnect = false) const;
private:
    _CRTIMP2 void __thiscall _Reset();
    _CRTIMP2 void __thiscall _Capture();
    _CRTIMP2 void __thiscall _Assign(void *_PContextCallback);
    _CRTIMP2 static bool __cdecl _IsCurrentOriginSTA();
    static const size_t _S_captureDeferred = 0;
    union {
        void *_M_pContextCallback;
        size_t _M_captureMethod;
    } _M_context;
};

class _ExceptionHolder {
    std::exception_ptr _M_stdException;
    _CRTIMP2 void __thiscall ReportUnhandledError();
};
}

class task_continuation_context : public details::_ContextCallback {
    _CRTIMP2 __thiscall task_continuation_context();
    bool _M_RunInline;
};

template <typename _ReturnType>
class task;
template <typename _Ty>
task<void> create_task(_Ty);

namespace details {
extern "C" __declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void *, unsigned long);
extern "C" __declspec(dllimport) int __stdcall CloseHandle(void *);
}

template <>
class task<void> {
public:
    task() noexcept = default;
    task(const task &o) noexcept : _M_State(o._M_State) { _Retain(); }
    task &operator=(const task &o) noexcept {
        if (this != &o) { _Release(); _M_State = o._M_State; _Retain(); }
        return *this;
    }
    ~task() { _Release(); }
    void wait() const {
        if (_M_State) details::WaitForSingleObject(_M_State->_Thread, 0xFFFFFFFFul);
    }

private:
    template <typename _Ty>
    friend task<void> create_task(_Ty);
    struct _State {
        long volatile _Refs;
        void *_Thread;
    };
    void _Retain() noexcept {
        if (_M_State) __atomic_add_fetch(&_M_State->_Refs, 1, __ATOMIC_SEQ_CST);
    }
    void _Release() noexcept {
        if (_M_State && __atomic_sub_fetch(&_M_State->_Refs, 1, __ATOMIC_SEQ_CST) == 0) {
            wait();
            details::CloseHandle(_M_State->_Thread);
            delete _M_State;
        }
        _M_State = nullptr;
    }
    _State *_M_State = nullptr;
};

template <typename _Ty>
task<void> create_task(_Ty _Fn) {
    struct _Run {
        static unsigned __stdcall _Go(void *_Arg) {
            _Ty *_F = static_cast<_Ty *>(_Arg);
            (*_F)();
            delete _F;
            return 0;
        }
    };
    task<void> _T;
    _T._M_State = new task<void>::_State{1, nullptr};
    _T._M_State->_Thread =
        reinterpret_cast<void *>(_beginthreadex(nullptr, 0, &_Run::_Go, new _Ty(static_cast<_Ty &&>(_Fn)), 0, nullptr));
    return _T;
}
}
#pragma pack(pop)
