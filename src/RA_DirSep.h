#ifndef RA_DIRSEP_H
#define RA_DIRSEP_H
#pragma once

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

#endif // !RA_DIRSEP_H
