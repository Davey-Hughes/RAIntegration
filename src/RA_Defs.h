#ifndef RA_DEFS_H
#define RA_DEFS_H
#pragma once

#include "util/Strings.hh"

#if !(RA_EXPORTS || RA_UTEST)
#include "windows_nodefines.h"
#include <Windows.h>
#include <WindowsX.h>

#pragma warning(push)
#pragma warning(disable : 4091)
#include <ShlObj.h>
#pragma warning(pop)

#include <tchar.h>

#include <cassert> 

#include <map>
#include <array> // algorithm, iterator, tuple
#include <string_view>
#include <queue> // deque, vector, algorithm

//	Version Information is integrated into tags
#else

#include "util/Log.hh"

//	RA-Only
using namespace std::string_literals;
#endif	// RA_EXPORTS

#include "data/Types.hh"
#include "data/Memory.hh"

/* Path separator. MSVC accepts '/' in most API calls but the tree's cached
 * paths are compared as strings, so they have to agree with what the platform's
 * IFileSystem produces. RALibretro carries the same pair (commit 15aa33b). */
#ifdef _WIN32
 #define RA_DIR_SEP    "\\"
 #define RA_DIR_SEP_L L"\\"
#else
 #define RA_DIR_SEP    "/"
 #define RA_DIR_SEP_L L"/"
#endif

#define RA_DIR_OVERLAY                  L"Overlay" RA_DIR_SEP_L
#define RA_DIR_BASE                     L"RACache" RA_DIR_SEP_L
#define RA_DIR_DATA                     RA_DIR_BASE L"Data" RA_DIR_SEP_L
#define RA_DIR_BADGE                    RA_DIR_BASE L"Badge" RA_DIR_SEP_L
#define RA_DIR_USERPIC                  RA_DIR_BASE L"UserPic" RA_DIR_SEP_L
#define RA_DIR_BOOKMARKS                RA_DIR_BASE L"Bookmarks" RA_DIR_SEP_L

#define RA_GAME_HASH_FILENAME           RA_DIR_DATA L"gamehashlibrary.txt"
#define RA_MY_PROGRESS_FILENAME         RA_DIR_DATA L"myprogress.txt"
#define RA_MY_GAME_LIBRARY_FILENAME     RA_DIR_DATA L"mygamelibrary.txt"

#define RA_NEWS_FILENAME                RA_DIR_DATA L"ra_news.txt"
#define RA_TITLES_FILENAME              RA_DIR_DATA L"gametitles.txt"
#define RA_LOG_FILENAME                 RA_DIR_DATA L"RALog.txt"

#define SIZEOF_ARRAY( ar )  ( sizeof( ar ) / sizeof( ar[ 0 ] ) )
#define SAFE_DELETE( x )    { if( x != nullptr ) { delete x; x = nullptr; } }

/* RARect and ResizeContent (Win32 dialog layout helpers) were removed here:
 * they had no callers anywhere in src/ or tests/, and their presence forced
 * every translation unit including RA_Defs.h to depend on <Windows.h>. */

namespace ra {
bool ParseUnsignedInt(const std::wstring& sValue, unsigned int nMaximumValue, _Out_ unsigned int& nValue, _Out_ std::wstring& sError);
bool ParseHex(const std::wstring& sValue, unsigned int nMaximumValue, _Out_ unsigned int& nValue, _Out_ std::wstring& sError);
bool ParseNumeric(const std::wstring& sValue, _Out_ unsigned int& nValue, _Out_ std::wstring& sError);
bool ParseFloat(const std::wstring& sValue, _Out_ float& fValue, _Out_ std::wstring& sError);
}

#ifndef UNUSED
#define UNUSED( x ) ( x );
#endif

#endif // !RA_DEFS_H
