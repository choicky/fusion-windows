#pragma once

// Windows SRW lock with the std::mutex shape.
//
// Why this exists at all: the TSF DLL is loaded into third-party processes, and an
// application that ships an older local MSVCP140 makes this DLL's CRT imports bind to
// that copy while its std::mutex objects were constructed with the newer headers this
// DLL is compiled against. The older `_Mtx_lock` then dereferences a field the newer
// construction never filled. Measured 2026-10-19 in Notepad-- 3.4 (MSVCP140 14.32): an
// access violation reading 0x0 at MSVCP140+0x13278 on the first diagnostic write, which
// is the text service's own constructor.
//
// SRW locks live in kernel32 and carry no CRT-owned object state, so they cannot take
// part in that mismatch. Code compiled into the TIP DLL must use this type (or no lock
// at all) instead of std::mutex, std::condition_variable or iostreams.

#include <windows.h>

namespace fusion::win32 {

class SrwMutex {
public:
  SrwMutex() = default;
  SrwMutex(const SrwMutex &) = delete;
  SrwMutex &operator=(const SrwMutex &) = delete;

  void lock() { AcquireSRWLockExclusive(&lock_); }
  void unlock() { ReleaseSRWLockExclusive(&lock_); }
  bool try_lock() { return TryAcquireSRWLockExclusive(&lock_) != 0; }

private:
  SRWLOCK lock_ = SRWLOCK_INIT;
};

} // namespace fusion::win32
