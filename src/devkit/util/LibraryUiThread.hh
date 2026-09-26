#ifndef RA_UTIL_LIBRARYUITHREAD_HH
#define RA_UTIL_LIBRARYUITHREAD_HH
#pragma once

namespace ra {
namespace util {

// The thread the library's own UI runs on when that is not the emulator's: off
// Windows, the Qt thread of an application the library owns
// (QtApplicationHost::RunOwnedThread). Host memory must never be touched there
// directly - it goes through DispatchMemoryRead - and EmulatorMemoryContext
// counts and reports any access that is. On Windows, and for a borrowed Qt
// application, no thread is marked: the UI runs on the emulator's thread.
inline thread_local bool t_bIsLibraryUiThread = false;

inline void MarkLibraryUiThread() noexcept { t_bIsLibraryUiThread = true; }

inline bool IsOnLibraryUiThread() noexcept { return t_bIsLibraryUiThread; }

} // namespace util
} // namespace ra

#endif // !RA_UTIL_LIBRARYUITHREAD_HH
