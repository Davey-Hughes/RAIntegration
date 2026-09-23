/* Definitions the test binary needs that no compiled source provides.
 *
 * Keep this small and keep each entry explained. Anything here is a symbol the
 * suite links against but the Linux build cannot yet produce; when a later task
 * makes the real thing available, the entry should go away.
 */

#include <cstdarg>
#include <cstdio>

/* rcheevos picks its compatibility branch in rc_compat.h from _MSC_VER, MinGW,
 * or __STDC_VERSION__. Under clang the first two are false and __STDC_VERSION__
 * is not defined in C++ at all, so every C++ translation unit that includes
 * rc_compat.h takes the C89 branch and gets `#define snprintf rc_snprintf`.
 * The C library does not: rc_compat.c is compiled as C17, so RC_C89_HELPERS is
 * off there and rc_snprintf is never emitted.
 *
 * The C89 fallback in rc_compat.c would call vsprintf and ignore the size
 * argument. That is not acceptable here - RAIntegration has ~55 snprintf and
 * sprintf_s call sites, most writing into fixed-size buffers, and msvc_compat.h
 * routes sprintf_s through snprintf as well. This definition is a plain
 * vsnprintf, which is what all of those call sites are written against.
 */
extern "C" int rc_snprintf(char* buffer, size_t size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int nResult = std::vsnprintf(buffer, size, format, args);
    va_end(args);
    return nResult;
}
