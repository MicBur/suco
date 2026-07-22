#pragma once
// Platform shims for building with MSVC. The codebase was written for MinGW,
// which provides POSIX names (popen, pclose, getpid, ssize_t, <unistd.h>) even
// under _WIN32; MSVC does not. These map the POSIX spellings onto their MSVC
// equivalents so the shared source compiles unchanged. Guarded by _MSC_VER only,
// so MinGW and POSIX builds are completely unaffected. Include this instead of
// <unistd.h> in translation units that need those names on both toolchains.
#ifdef _MSC_VER
    #include <process.h>   // _getpid
    #include <cstdio>      // _popen, _pclose
    #include <BaseTsd.h>   // SSIZE_T
    using ssize_t = SSIZE_T;
    #ifndef popen
        #define popen _popen
    #endif
    #ifndef pclose
        #define pclose _pclose
    #endif
    #ifndef getpid
        #define getpid _getpid
    #endif
#else
    #include <unistd.h>
#endif
