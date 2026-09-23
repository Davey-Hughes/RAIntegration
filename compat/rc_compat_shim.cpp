/* Definitions rcheevos expects to exist but never provides, on this build.
 *
 * rcheevos picks its compatibility branch in rc_compat.h from _MSC_VER, MinGW,
 * or __STDC_VERSION__. Under clang the first two are false and __STDC_VERSION__
 * is not defined in C++ at all, so every C++ translation unit that includes
 * rc_compat.h takes the C89 branch and gets `#define snprintf rc_snprintf`.
 * The C library does not: rc_compat.c is compiled as C17, so RC_C89_HELPERS is
 * off there and rc_snprintf is never emitted. That leaves every C++ object in
 * the product build - not just the tests - with an undefined reference.
 *
 * This is a product source, not a test stub: ra_portable's own objects
 * (e.g. AchievementRuntime.cpp) call the renamed snprintf, so anything that
 * links ra_portable needs this definition, with or without the test sources.
 *
 * The C89 fallback in rc_compat.c would call vsprintf and ignore the size
 * argument. That is not acceptable here - RAIntegration has ~14 product call
 * sites that write into fixed-size buffers via (s)printf, and msvc_compat.h
 * routes sprintf_s through snprintf as well. This definition is a plain
 * bounded vsnprintf, which is what all of those call sites are written
 * against.
 */

#include <cstdarg>
#include <cstdio>

extern "C" int rc_snprintf(char* buffer, size_t size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int nResult = std::vsnprintf(buffer, size, format, args);
    va_end(args);
    return nResult;
}
