/* libmpg123 for Windows 10 Mobile (MSVC, 32-bit ARM): what ports/cmake's configure step would
 * find, written down, because its checks link test programs and this is a cross build.
 *
 * The generic C decoder (mpg123 has NEON code only as GNU assembler), and narrow file names:
 * WANT_WIN32_UNICODE would bring in shlwapi, and the game hands over the same ANSI paths it
 * opens everything else with. */
#ifndef MPG123_W10M_CONFIG_H
#define MPG123_W10M_CONFIG_H

#define ACCURATE_ROUNDING 1
#define FRAME_INDEX 1
#define INDEX_SIZE 1000
#define GAPLESS 1
#define IEEE_FLOAT 1
#define USE_NEW_HUFFTABLE 1

#define HAVE_ATOLL 1
#define HAVE_DIRENT_H 1 /* dirent.h next to this file */
#define HAVE_INTTYPES_H 1
#define HAVE_LOCALE_H 1
#define HAVE_SETLOCALE 1
#define HAVE_SIGNAL_H 1
#define HAVE_STRERROR 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_WINDOWS_H 1

/* nothing prints: there is no console, and the game reports a stream that fails to open */
#define NO_WARNING 1
#define NO_ERRORMSG 1
#define NO_ERETURN 1

#define PACKAGE_NAME "mpg123"
#define PACKAGE_VERSION "1.32.10"
#define DEFAULT_OUTPUT_MODULE "dummy"
#define LT_MODULE_EXT ".dll"
#define PKGLIBDIR "."

/* off_t is a long here; the 64-bit entry points use _lseeki64 (lfs_wrap.c) */
#define SIZEOF_OFF_T 4

#define STDERR_FILENO (_fileno(stderr))
#define STDIN_FILENO (_fileno(stdin))
#define STDOUT_FILENO (_fileno(stdout))

#define strcasecmp _stricmp
#define strncasecmp _strnicmp
/* <sys/stat.h> of the UCRT has the bits but not the POSIX tests */
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)

#endif
