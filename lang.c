#include "vtree.h"
#include "lang.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>

int  lang_file_count = 0;
char lang_names[MAX_LANG_FILES][MAX_LANG_NAME];
int  current_lang_idx = 0;

typedef struct { char key[MAX_LANG_KEY_LEN]; char val[MAX_LANG_VAL_LEN]; } LangEntry;
static LangEntry lang_table[MAX_LANG_KEYS];
static int       lang_entry_count = 0;

// ---------------------------------------------------------------------------
// Build the path to the lang/ directory next to the executable.
// ---------------------------------------------------------------------------
static void get_lang_dir(char *out) {
    char exe_buf[MAX_PATH];
    ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (len > 0) {
        exe_buf[len] = '\0';
        char *slash = strrchr(exe_buf, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, MAX_PATH, "%s/lang", exe_buf);
            return;
        }
    }
    snprintf(out, MAX_PATH, "lang");
}

// ---------------------------------------------------------------------------
// Parse one language INI file into the flat lookup table.
// Sections are ignored — all keys are merged into one flat namespace.
// ---------------------------------------------------------------------------
static void lang_load_file_path(const char *path) {
    lang_entry_count = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[MAX_LANG_VAL_LEN + MAX_LANG_KEY_LEN + 4];
    while (fgets(line, sizeof(line), f)) {
        // Trim trailing newline
        int l = (int)strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';

        char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '\0' || *t == '#' || *t == '[') continue;

        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';

        // Trim key
        char *ke = eq - 1;
        while (ke >= t && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';

        // Trim value
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;

        if (lang_entry_count >= MAX_LANG_KEYS) break;
        strncpy(lang_table[lang_entry_count].key, t,  MAX_LANG_KEY_LEN - 1);
        lang_table[lang_entry_count].key[MAX_LANG_KEY_LEN - 1] = '\0';
        strncpy(lang_table[lang_entry_count].val, v,  MAX_LANG_VAL_LEN - 1);
        lang_table[lang_entry_count].val[MAX_LANG_VAL_LEN - 1] = '\0';
        lang_entry_count++;
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// Scan the lang/ directory and populate lang_names[].
// ---------------------------------------------------------------------------
static int lang_name_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static void scan_lang_files(void) {
    lang_file_count = 0;
    char dir_path[MAX_PATH];
    get_lang_dir(dir_path);

    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) && lang_file_count < MAX_LANG_FILES) {
        const char *n = e->d_name;
        size_t nl = strlen(n);
        if (nl < 5) continue;
        if (strcmp(n + nl - 4, ".ini") != 0) continue;
        // Strip .ini extension for display name
        int nlen = (int)(nl - 4);
        if (nlen <= 0 || nlen >= MAX_LANG_NAME) continue;
        strncpy(lang_names[lang_file_count], n, nlen);
        lang_names[lang_file_count][nlen] = '\0';
        lang_file_count++;
    }
    closedir(d);

    qsort(lang_names, lang_file_count, MAX_LANG_NAME, lang_name_cmp);
}

// ---------------------------------------------------------------------------
// Find the index of a language name in lang_names[].
// Returns 0 (first entry) if not found.
// ---------------------------------------------------------------------------
static int find_lang_idx(const char *name) {
    for (int i = 0; i < lang_file_count; i++)
        if (strcmp(lang_names[i], name) == 0) return i;
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void lang_init(void) {
    scan_lang_files();
    current_lang_idx = find_lang_idx(cfg.language_name);
    lang_reload();
}

void lang_reload(void) {
    if (lang_file_count == 0) { lang_entry_count = 0; return; }
    char dir_path[MAX_PATH];
    get_lang_dir(dir_path);
    char file_path[MAX_PATH];
    snprintf(file_path, MAX_PATH, "%s/%s.ini", dir_path, lang_names[current_lang_idx]);
    lang_load_file_path(file_path);
    // Sync cfg.language_name to the loaded language
    strncpy(cfg.language_name, lang_names[current_lang_idx], MAX_LANG_NAME - 1);
    cfg.language_name[MAX_LANG_NAME - 1] = '\0';
}

const char *tr(const char *key) {
    for (int i = 0; i < lang_entry_count; i++)
        if (strcmp(lang_table[i].key, key) == 0)
            return lang_table[i].val;
    return key;  // fallback: show the key itself
}
