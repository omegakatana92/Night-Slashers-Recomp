/*
 * Night Slashers native runtime: host file-system helpers.
 *
 * All paths are UTF-8. On Windows they are converted to UTF-16 for the CRT, so paths
 * with spaces and non-ASCII characters work on both platforms.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef NSL_PLATFORM_H
#define NSL_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

FILE *nsl_fopen(const char *path, const char *mode);
uint8_t *nsl_read_file(const char *path, size_t *size);
/* Writes to path.tmp and renames over path, so a crash never leaves a torn file. */
int nsl_write_file_atomic(const char *path, const void *data, size_t size);
int nsl_path_is_directory(const char *path);
int nsl_path_exists(const char *path);
int nsl_make_directories(const char *path);
int nsl_remove_file(const char *path);

/* Lists directory entries (UTF-8 names, no "." or ".."). Returns the count, or -1.
 * The callback receives each name and whether it is a directory. */
typedef void (*NslDirCallback)(void *user, const char *name, int is_directory);
int nsl_list_directory(const char *path, NslDirCallback callback, void *user);

/* Per-user writable data directory, e.g.
 *   Windows: %APPDATA%\NightSlashersRecomp
 *   Linux:   $XDG_DATA_HOME/NightSlashersRecomp or ~/.local/share/NightSlashersRecomp
 * Never inside an AppImage's read-only mount. Created on first use. */
const char *nsl_user_data_dir(void);

/* Joins a and b with the platform separator into out. */
void nsl_path_join(char *out, size_t size, const char *a, const char *b);

#endif
