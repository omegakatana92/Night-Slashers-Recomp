/*
 * Night Slashers native runtime: host file-system helpers.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/platform.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static wchar_t *widen(const char *text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (count <= 0) return NULL;
    wchar_t *wide = (wchar_t *)malloc((size_t)count * sizeof(wchar_t));
    if (wide) MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, count);
    return wide;
}

static void narrow(const wchar_t *wide, char *out, size_t size) {
    if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)size, NULL, NULL)) {
        if (size) out[0] = 0;
    }
}
#endif

FILE *nsl_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t *wpath = widen(path);
    wchar_t *wmode = widen(mode);
    FILE *file = (wpath && wmode) ? _wfopen(wpath, wmode) : NULL;
    free(wpath);
    free(wmode);
    return file;
#else
    return fopen(path, mode);
#endif
}

uint8_t *nsl_read_file(const char *path, size_t *size) {
    FILE *file = nsl_fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length < 0) { fclose(file); return NULL; }
    fseek(file, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(length > 0 ? (size_t)length : 1);
    if (!data) { fclose(file); return NULL; }
    if (length > 0 && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    if (size) *size = (size_t)length;
    return data;
}

int nsl_write_file_atomic(const char *path, const void *data, size_t size) {
    char temp[1100];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE *file = nsl_fopen(temp, "wb");
    if (!file) return -1;
    size_t put = fwrite(data, 1, size, file);
    int ok = (fflush(file) == 0) && put == size;
    if (fclose(file) != 0) ok = 0;
    if (!ok) { nsl_remove_file(temp); return -1; }
#ifdef _WIN32
    wchar_t *wfrom = widen(temp), *wto = widen(path);
    BOOL moved = (wfrom && wto) ? MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) : FALSE;
    free(wfrom);
    free(wto);
    if (!moved) { nsl_remove_file(temp); return -1; }
#else
    if (rename(temp, path) != 0) { nsl_remove_file(temp); return -1; }
#endif
    return 0;
}

int nsl_path_is_directory(const char *path) {
#ifdef _WIN32
    wchar_t *wide = widen(path);
    DWORD attributes = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

int nsl_path_exists(const char *path) {
#ifdef _WIN32
    wchar_t *wide = widen(path);
    DWORD attributes = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES;
#else
    struct stat info;
    return stat(path, &info) == 0;
#endif
}

int nsl_remove_file(const char *path) {
#ifdef _WIN32
    wchar_t *wide = widen(path);
    int result = wide ? _wremove(wide) : -1;
    free(wide);
    return result;
#else
    return remove(path);
#endif
}

static int make_one(const char *path) {
#ifdef _WIN32
    wchar_t *wide = widen(path);
    int result = wide ? _wmkdir(wide) : -1;
    free(wide);
    return (result == 0 || nsl_path_is_directory(path)) ? 0 : -1;
#else
    return (mkdir(path, 0755) == 0 || errno == EEXIST) ? 0 : -1;
#endif
}

int nsl_make_directories(const char *path) {
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "%s", path);
    size_t length = strlen(buffer);
    for (size_t i = 1; i < length; i++) {
        if (buffer[i] == '/' || buffer[i] == '\\') {
            if (i == 2 && buffer[1] == ':') continue;        /* C:\ */
            char saved = buffer[i];
            buffer[i] = 0;
            make_one(buffer);
            buffer[i] = saved;
        }
    }
    return make_one(buffer);
}

int nsl_list_directory(const char *path, NslDirCallback callback, void *user) {
    int count = 0;
#ifdef _WIN32
    char pattern[1100];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    wchar_t *wide = widen(pattern);
    if (!wide) return -1;
    WIN32_FIND_DATAW data;
    HANDLE handle = FindFirstFileW(wide, &data);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        char name[768];
        narrow(data.cFileName, name, sizeof(name));
        callback(user, name, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        count++;
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
#else
    DIR *dir = opendir(path);
    if (!dir) return -1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char full[1400];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        callback(user, entry->d_name, nsl_path_is_directory(full));
        count++;
    }
    closedir(dir);
#endif
    return count;
}

void nsl_path_join(char *out, size_t size, const char *a, const char *b) {
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    size_t length = strlen(a);
    if (length && (a[length - 1] == '/' || a[length - 1] == '\\')) {
        snprintf(out, size, "%s%s", a, b);
    } else {
        snprintf(out, size, "%s%c%s", a, sep, b);
    }
}

const char *nsl_user_data_dir(void) {
    static char path[1024];
    if (path[0]) return path;
    const char *override = getenv("NSL_DATA_DIR");
    if (override && override[0]) {
        snprintf(path, sizeof(path), "%s", override);
    } else {
#ifdef _WIN32
        wchar_t *appdata = _wgetenv(L"APPDATA");
        char base[900] = ".";
        if (appdata) narrow(appdata, base, sizeof(base));
        snprintf(path, sizeof(path), "%s\\NightSlashersRecomp", base);
#else
        const char *xdg = getenv("XDG_DATA_HOME");
        const char *home = getenv("HOME");
        if (xdg && xdg[0]) {
            snprintf(path, sizeof(path), "%s/NightSlashersRecomp", xdg);
        } else if (home && home[0]) {
            snprintf(path, sizeof(path), "%s/.local/share/NightSlashersRecomp", home);
        } else {
            snprintf(path, sizeof(path), "./NightSlashersRecomp");
        }
#endif
    }
    nsl_make_directories(path);
    return path;
}
