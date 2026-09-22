#ifndef RA_MSVC_COMPAT_H
#define RA_MSVC_COMPAT_H

/* Compatibility shim for building RAIntegration with non-MSVC toolchains.
 * MSVC provides these as intrinsic macros (SAL annotations from sal.h and
 * _NODISCARD from the MS STL); on clang/gcc they need definitions. */

#ifndef _MSC_VER

#ifndef _NODISCARD
#define _NODISCARD [[nodiscard]]
#endif

/* SAL source-annotation macros: documentation only, no codegen effect. */
#define _In_
#define _In_z_
#define _In_opt_
#define _Out_
#define _Out_opt_
#define _Inout_
#define _Inout_opt_
#define _Success_(expr)
#define _Printf_format_string_
#define _Use_decl_annotations_


/* MSVC pulls these in transitively through its own headers; clang/libstdc++ do not. */
/* The STL set that src/pch.h (force-included by the MSVC project) provides. */
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

/* MSVC gets these from the forced-include pch.h; mirror that here. */
#include <gsl/gsl>

/* MSVC-internal helper macros used in the source. */
#ifndef _FALLTHROUGH
#define _FALLTHROUGH [[fallthrough]]
#endif
#ifndef __FUNC__
#define __FUNC__ __FUNCTION__
#endif
#ifndef _CRT_WIDE
#define _CRT_WIDE_(s) L##s
#define _CRT_WIDE(s) _CRT_WIDE_(s)
#endif


/* MSVC's wide-character assert. glibc has no equivalent, so route it to the
 * narrow one. */
#include <cassert>
#include <cwchar>
#include <string>
inline void _wassert(const wchar_t* pMessage, const wchar_t* pFile, unsigned nLine)
{
    std::mbstate_t state{};
    const wchar_t* pSrc = pMessage;
    const std::size_t nLength = std::wcsrtombs(nullptr, &pSrc, 0, &state);
    std::string sMessage = (nLength == static_cast<std::size_t>(-1)) ? "assertion failed" : std::string(nLength, '\0');
    if (nLength != static_cast<std::size_t>(-1))
    {
        pSrc = pMessage;
        state = std::mbstate_t{};
        std::wcsrtombs(sMessage.data(), &pSrc, nLength, &state);
    }

    pSrc = pFile;
    state = std::mbstate_t{};
    const std::size_t nFileLength = std::wcsrtombs(nullptr, &pSrc, 0, &state);
    std::string sFile = (nFileLength == static_cast<std::size_t>(-1)) ? "<file>" : std::string(nFileLength, '\0');
    if (nFileLength != static_cast<std::size_t>(-1))
    {
        pSrc = pFile;
        state = std::mbstate_t{};
        std::wcsrtombs(sFile.data(), &pSrc, nFileLength, &state);
    }

    __assert_fail(sMessage.c_str(), sFile.c_str(), nLine, "");
}


/* ---- Win32 types that leak through the DLL export surface ----
 * Exports.hh and RAInterface/RA_Interface.h type the emulator-facing API in
 * Win32 terms. Modelling the window handle as an opaque pointer lets the rest
 * of the codebase compile; a real port needs a platform-neutral interface. */
struct HWND__;
using HWND = HWND__*;
struct HMENU__;
using HMENU = HMENU__*;
using BOOL = int;
using DWORD = unsigned long;
using LONG = long;
using BYTE = unsigned char;
using LPARAM = long;
using WPARAM = unsigned long;
using UINT = unsigned int;
using WORD = unsigned short;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;
using LPCSTR = const char*;
using LPSTR = char*;
using HANDLE = void*;
struct HINSTANCE__;
using HINSTANCE = HINSTANCE__*;

/* ---- MSVC CRT extensions ---- */
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <ctime>
#include <chrono>
#include <thread>

#ifndef __fallthrough
#define __fallthrough [[fallthrough]]
#endif

#define swprintf_s swprintf
#define sprintf_s snprintf

inline int localtime_s(std::tm* pResult, const std::time_t* pTime)
{
    return (::localtime_r(pTime, pResult) == nullptr) ? 1 : 0;
}

inline int _wtoi(const wchar_t* pString) noexcept
{
    return static_cast<int>(std::wcstol(pString, nullptr, 10));
}

inline void Sleep(unsigned long nMilliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(nMilliseconds));
}

#endif /* !_MSC_VER */

#endif /* RA_MSVC_COMPAT_H */
