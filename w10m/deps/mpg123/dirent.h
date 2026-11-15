/* compat.c of mpg123 lists directories for its output modules, with <dirent.h> or, on Windows,
 * with shlwapi's wide paths (WANT_WIN32_UNICODE, which this build leaves off). libmpg123 never
 * calls those functions: here they only have to compile. */
#ifndef MPG123_W10M_DIRENT_H
#define MPG123_W10M_DIRENT_H

typedef struct DIR DIR;
struct dirent {
    char d_name[260];
};

static DIR* opendir(const char* path) { (void)path; return 0; }
static struct dirent* readdir(DIR* dir) { (void)dir; return 0; }
static int closedir(DIR* dir) { (void)dir; return 0; }

#endif
