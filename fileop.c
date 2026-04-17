#include "vtree.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

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
    strncpy(s->current_path, path, MAX_PATH);
    s->file_count = 0; s->selected_index = 0; s->scroll_offset = 0;

    if (strcmp(path, "/") != 0) {
        strcpy(s->files[0].name, "..");
        s->files[0].is_dir = true; s->files[0].is_link = false; s->files[0].size = 0; s->files[0].marked = false;
        s->file_count = 1;
    }

    struct dirent* e;
    while ((e = readdir(dir)) && s->file_count < MAX_FILES) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!cfg.show_hidden && e->d_name[0] == '.') continue;
        char full[MAX_PATH];
        join_path(full, path, e->d_name);   // fixed: no // at root
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

// Returns 0 on full success, -1 if any file failed
int copy_path(const char* src, const char* dest) {
    struct stat st;
    if (stat(src, &st) != 0) return -1;

    if (S_ISDIR(st.st_mode)) {
        vtree_log("[copy] mkdir %s\n", dest);
        if (mkdir(dest, 0755) != 0)
            vtree_log("[copy] mkdir FAILED: %s (errno %d: %s)\n", dest, errno, strerror(errno));
        DIR* dir = opendir(src);
        if (!dir) { vtree_log("[copy] opendir FAILED: %s (errno %d: %s)\n", src, errno, strerror(errno)); return -1; }
        struct dirent* e;
        int err = 0;
        while ((e = readdir(dir))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char s_sub[MAX_PATH], d_sub[MAX_PATH];
            join_path(s_sub, src,  e->d_name);
            join_path(d_sub, dest, e->d_name);
            if (copy_path(s_sub, d_sub) != 0) err = -1;  // propagate errors
        }
        closedir(dir);
        return err;
    }

    vtree_log("[copy] %s -> %s\n", src, dest);
    FILE *fs = fopen(src,  "rb");
    FILE *fd = fopen(dest, "wb");
    if (!fs || !fd) {
        vtree_log("[copy] fopen FAILED: %s (errno %d: %s)\n",
                  !fs ? src : dest, errno, strerror(errno));
        if (fs) fclose(fs); if (fd) fclose(fd); return -1;
    }

    char buf[32768]; size_t n;
    int err = 0;
    while ((n = fread(buf, 1, sizeof(buf), fs)) > 0) {
        if (fwrite(buf, 1, n, fd) != n) {
            vtree_log("[copy] write FAILED: %s (errno %d: %s)\n", dest, errno, strerror(errno));
            err = -1; break;
        }
    }
    fclose(fs); fclose(fd);
    return err;
}

int delete_path(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;

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

    vtree_log("[delete] unlink %s\n", path);
    int r = unlink(path);
    if (r != 0) vtree_log("[delete] unlink FAILED: %s (errno %d: %s)\n", path, errno, strerror(errno));
    return r;
}
