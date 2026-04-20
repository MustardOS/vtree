#include "vtree.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#ifdef __linux__
#  include <sys/syscall.h>
#  ifdef SYS_copy_file_range
#    define HAVE_CFR 1
static ssize_t do_cfr(int in, off_t *off_in, int out, off_t *off_out, size_t len) {
    return syscall(SYS_copy_file_range, in, off_in, out, off_out, len, 0);
}
#  endif
#endif

char* trim(char* str) {
    char* end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

// Safe path join: avoids double-slash when dir is "/"
void join_path(char* out, const char* dir, const char* name) {
    if (strcmp(dir, "/") == 0)
        snprintf(out, MAX_PATH, "/%s", name);
    else
        snprintf(out, MAX_PATH, "%s/%s", dir, name);
}

void format_size(long long bytes, char* out) {
    if      (bytes < 1024)             snprintf(out, 16, "%lld B",   bytes);
    else if (bytes < 1024 * 1024)      snprintf(out, 16, "%.1f KB",  bytes / 1024.0);
    else if (bytes < 1024 * 1024 * 1024) snprintf(out, 16, "%.1f MB", bytes / 1048576.0);
    else                               snprintf(out, 16, "%.1f GB",  bytes / 1073741824.0);
}

static int sort_files(const void* a, const void* b) {
    const FileEntry* f1 = (const FileEntry*)a;
    const FileEntry* f2 = (const FileEntry*)b;
    if  (f1->is_dir && !f2->is_dir) return -1;
    if (!f1->is_dir &&  f2->is_dir) return  1;
    return strcasecmp(f1->name, f2->name);
}

void load_dir(int p_idx, const char* path) {
    vtree_log("[load_dir] pane %d: %s\n", p_idx + 1, path);
    DIR* dir = opendir(path);
    if (!dir) { vtree_log("[load_dir] opendir FAILED: %s (errno %d: %s)\n", path, errno, strerror(errno)); return; }

    AppState* s = &panes[p_idx];

    int cap = (s->file_capacity > 0) ? s->file_capacity : FILES_INIT_CAP;
    FileEntry *buf = realloc(s->files, cap * sizeof(FileEntry));
    if (!buf) {
        vtree_log("[load_dir] alloc FAILED (cap=%d)\n", cap);
        closedir(dir);
        return;
    }
    s->files = buf;
    s->file_capacity = cap;

    strncpy(s->current_path, path, MAX_PATH);
    s->file_count = 0; s->selected_index = 0; s->scroll_offset = 0;

    if (strcmp(path, "/") != 0) {
        strcpy(s->files[0].name, "..");
        s->files[0].is_dir = true; s->files[0].is_link = false; s->files[0].size = 0; s->files[0].marked = false;
        s->file_count = 1;
    }

    struct dirent* e;
    while ((e = readdir(dir))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!cfg.show_hidden && e->d_name[0] == '.') continue;

        if (s->file_count >= s->file_capacity) {
            int new_cap = s->file_capacity * 2;
            FileEntry *grown = realloc(s->files, new_cap * sizeof(FileEntry));
            if (!grown) {
                vtree_log("[load_dir] realloc FAILED at %d entries, stopping here\n", s->file_count);
                break;
            }
            s->files = grown;
            s->file_capacity = new_cap;
        }

        char full[MAX_PATH];
        join_path(full, path, e->d_name);
        struct stat lst, st;
        if (lstat(full, &lst) == 0) {
            bool link = S_ISLNK(lst.st_mode);
            struct stat *info = &lst;
            if (link && stat(full, &st) == 0) info = &st;
            strncpy(s->files[s->file_count].name, e->d_name, 255);
            s->files[s->file_count].is_dir  = S_ISDIR(info->st_mode);
            s->files[s->file_count].is_link = link;
            s->files[s->file_count].size    = (long long)info->st_size;
            s->files[s->file_count].marked  = false;
            s->file_count++;
        }
    }
    closedir(dir);

    int start = (s->file_count > 0 && strcmp(s->files[0].name, "..") == 0) ? 1 : 0;
    qsort(&s->files[start], s->file_count - start, sizeof(FileEntry), sort_files);
    vtree_log("[load_dir] pane %d: %d entries loaded\n", p_idx + 1, s->file_count);
}

// ---- copy_path helpers ----

// Copies file data from src to dst. Tries copy_file_range (kernel 4.5+, stays in-kernel)
// and falls back to a read/write loop on ENOSYS/EXDEV/EINVAL (e.g. kernel 4.4, cross-device).
static int copy_file_data(FILE *src, FILE *dst, off_t file_size) {
#ifdef HAVE_CFR
    if (file_size > 0) {
        off_t off_in = 0, off_out = 0;
        ssize_t r = do_cfr(fileno(src), &off_in, fileno(dst), &off_out, (size_t)file_size);
        if (r >= 0 || (errno != ENOSYS && errno != EXDEV && errno != EINVAL)) {
            if (r < 0) return -1;
            off_t remaining = file_size - r;
            while (remaining > 0) {
                r = do_cfr(fileno(src), &off_in, fileno(dst), &off_out, (size_t)remaining);
                if (r <= 0) return r < 0 ? -1 : 0;
                remaining -= r;
            }
            return 0;
        }
        // Not supported — seek back and fall through to read/write
        rewind(src);
        rewind(dst);
    }
#else
    (void)file_size;
#endif
    char buf[32768];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) return -1;
    }
    return ferror(src) ? -1 : 0;
}

// (dev, ino) pairs for cycle detection during directory traversal
#define MAX_VISIT 512
typedef struct { dev_t dev; ino_t ino; } InodePair;

static int copy_path_r(const char *src, const char *dest,
                       InodePair *vis, int *nvis);

// Returns 0 on full success, -1 if any file failed
int copy_path(const char* src, const char* dest) {
    InodePair vis[MAX_VISIT];
    int nvis = 0;
    return copy_path_r(src, dest, vis, &nvis);
}

static int copy_path_r(const char *src, const char *dest,
                       InodePair *vis, int *nvis) {
    struct stat st;
    if (lstat(src, &st) != 0) return -1;

    // Symlink: recreate without following — prevents cycles and preserves links
    if (S_ISLNK(st.st_mode)) {
        char target[MAX_PATH];
        ssize_t n = readlink(src, target, sizeof(target) - 1);
        if (n < 0) {
            vtree_log("[copy] readlink FAILED: %s (errno %d: %s)\n", src, errno, strerror(errno));
            return -1;
        }
        target[n] = '\0';
        if (symlink(target, dest) != 0) {
            vtree_log("[copy] symlink FAILED: %s -> %s (errno %d: %s)\n",
                      dest, target, errno, strerror(errno));
            return -1;
        }
        return 0;
    }

    if (S_ISDIR(st.st_mode)) {
        // Cycle detection: refuse to re-enter a directory inode we've already visited
        for (int i = 0; i < *nvis; i++) {
            if (vis[i].dev == st.st_dev && vis[i].ino == st.st_ino) {
                vtree_log("[copy] cycle detected, skipping: %s\n", src);
                return 0;
            }
        }
        if (*nvis < MAX_VISIT) {
            vis[*nvis].dev = st.st_dev;
            vis[*nvis].ino = st.st_ino;
            (*nvis)++;
        }

        vtree_log("[copy] mkdir %s\n", dest);
        if (mkdir(dest, st.st_mode & 07777) != 0 && errno != EEXIST)
            vtree_log("[copy] mkdir FAILED: %s (errno %d: %s)\n", dest, errno, strerror(errno));

        DIR *dir = opendir(src);
        if (!dir) {
            vtree_log("[copy] opendir FAILED: %s (errno %d: %s)\n", src, errno, strerror(errno));
            return -1;
        }
        struct dirent *e;
        int err = 0;
        while ((e = readdir(dir))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            if (strlen(src) + strlen(e->d_name) + 2 >= MAX_PATH) {
                vtree_log("[copy] path too long, skipping: %s/%s\n", src, e->d_name);
                err = -1;
                continue;
            }
            char s_sub[MAX_PATH], d_sub[MAX_PATH];
            join_path(s_sub, src,  e->d_name);
            join_path(d_sub, dest, e->d_name);
            if (copy_path_r(s_sub, d_sub, vis, nvis) != 0) err = -1;
        }
        closedir(dir);

        // Restore dir timestamps after all children are written
        struct timespec times[2] = { st.st_atim, st.st_mtim };
        utimensat(AT_FDCWD, dest, times, 0);
        return err;
    }

    // Regular file (or other non-dir, non-link types)
    vtree_log("[copy] %s -> %s\n", src, dest);

    // Build temp path for atomic write; on success we rename into place
    char tmp[MAX_PATH];
    if (snprintf(tmp, sizeof(tmp), "%s.vtree_tmp", dest) >= (int)sizeof(tmp)) {
        vtree_log("[copy] temp path too long: %s\n", dest);
        return -1;
    }

    FILE *fs = fopen(src, "rb");
    if (!fs) {
        vtree_log("[copy] fopen FAILED: %s (errno %d: %s)\n", src, errno, strerror(errno));
        return -1;
    }
    struct stat src_stat;
    fstat(fileno(fs), &src_stat);

    FILE *fd = fopen(tmp, "wb");
    if (!fd) {
        vtree_log("[copy] fopen FAILED: %s (errno %d: %s)\n", tmp, errno, strerror(errno));
        fclose(fs);
        return -1;
    }

    int err = copy_file_data(fs, fd, src_stat.st_size);
    fclose(fs);
    if (fclose(fd) != 0) {
        vtree_log("[copy] fclose FAILED: %s (errno %d: %s)\n", tmp, errno, strerror(errno));
        err = -1;
    }

    if (err != 0) {
        unlink(tmp);
        return -1;
    }

    // Restore permissions and timestamps before moving into place
    chmod(tmp, src_stat.st_mode & 07777);
    struct timespec times[2] = { src_stat.st_atim, src_stat.st_mtim };
    utimensat(AT_FDCWD, tmp, times, 0);

    if (rename(tmp, dest) != 0) {
        vtree_log("[copy] rename FAILED: %s -> %s (errno %d: %s)\n",
                  tmp, dest, errno, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

int delete_path(const char* path) {
    struct stat st;
    // lstat: don't follow symlinks — a symlink to a dir must be unlinked, not recursed into
    if (lstat(path, &st) != 0) return -1;

    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path);
        if (!dir) return -1;
        struct dirent* e;
        while ((e = readdir(dir))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char sub[MAX_PATH];
            join_path(sub, path, e->d_name);
            delete_path(sub);
        }
        closedir(dir);
        vtree_log("[delete] rmdir %s\n", path);
        int r = rmdir(path);
        if (r != 0) vtree_log("[delete] rmdir FAILED: %s (errno %d: %s)\n", path, errno, strerror(errno));
        return r;
    }

    // Files and symlinks: unlink removes both
    vtree_log("[delete] unlink %s\n", path);
    int r = unlink(path);
    if (r != 0) vtree_log("[delete] unlink FAILED: %s (errno %d: %s)\n", path, errno, strerror(errno));
    return r;
}
