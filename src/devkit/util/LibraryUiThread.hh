#ifndef RA_UTIL_LIBRARYUITHREAD_HH
#define RA_UTIL_LIBRARYUITHREAD_HH
#pragma once

namespace ra {
namespace util {

// The thread the library's own UI runs on when that is not the emulator's: off
// Windows, the Qt thread of an application the library owns
// (QtApplicationHost::RunOwnedThread). Host memory must never be touched there
// directly - it goes through DispatchMemoryRead or DispatchMemoryWrite - and
// EmulatorMemoryContext counts and reports any access that is. On Windows no
// thread is marked: the UI runs on the emulator's thread. Nor is a borrowed Qt
// application's thread, which is the host's own: touching memory there directly
// is safe only if the host also runs its frames on it. A host that emulates on
// another thread is not checked, though routing still takes the dispatched
// paths to its frame thread.
inline thread_local bool t_bIsLibraryUiThread = false;

inline void MarkLibraryUiThread() noexcept { t_bIsLibraryUiThread = true; }

inline bool IsOnLibraryUiThread() noexcept { return t_bIsLibraryUiThread; }

} // namespace util
} // namespace ra

#endif // !RA_UTIL_LIBRARYUITHREAD_HH
