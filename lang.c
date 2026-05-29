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

// English reference table — the trusted baseline used to validate the
// printf-specifier signatures of whatever language is active.
static LangEntry lang_baseline[MAX_LANG_KEYS];
static int       lang_baseline_count = 0;

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
static void load_ini_into(const char *path, LangEntry *tbl, int *cnt) {
    *cnt = 0;
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

        if (*cnt >= MAX_LANG_KEYS) break;
        copy_str(tbl[*cnt].key, t, MAX_LANG_KEY_LEN);
        copy_str(tbl[*cnt].val, v, MAX_LANG_VAL_LEN);
        (*cnt)++;
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// Format-string safety. Translations are user-supplied files yet are used as
// printf formats at 60+ call sites, so a stray %n (writes through a pointer
// arg) or a wrong specifier count/type (varargs over-read → crash / info leak)
// would be undefined behaviour. We defend once at load time, leaving the hot
// tr() path and every call site untouched.
//
// fmt_signature() rejects the constructs that are never valid in an untrusted
// format — %n, and '*' dynamic width/precision (consumes an extra arg) — and
// otherwise fills 'out' with a compact signature of the argument types a format
// consumes: one token per conversion (length-modifier + conversion letter),
// '%%' consuming nothing. Harmless flags/width/precision are ignored since they
// don't change which argument is read. Equal signatures ⇒ identical arg lists.
// ---------------------------------------------------------------------------
static bool fmt_signature(const char *s, char *out, size_t outcap) {
    size_t o = 0;
    out[0] = '\0';
    for (const char *p = s; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;                      // literal %%
        if (*p == '\0') return false;                 // trailing '%'
        while (*p && strchr("-+ 0#", *p)) p++;         // flags
        if (*p == '*') return false;                  // dynamic width → extra arg
        while (*p >= '0' && *p <= '9') p++;            // width
        if (*p == '.') {                              // precision
            p++;
            if (*p == '*') return false;
            while (*p >= '0' && *p <= '9') p++;
        }
        char lenmod[3] = {0}; int li = 0;             // length modifier (h/l/ll/…)
        while (*p && strchr("hljztL", *p) && li < 2) lenmod[li++] = *p++;
        char conv = *p;
        if (conv == '\0' || conv == 'n')           return false;   // %n never allowed
        if (!strchr("diouxXeEfFgGaAcs", conv))     return false;   // unknown conversion
        char tok[8];
        int tl = snprintf(tok, sizeof(tok), "%s%c|", lenmod, conv);
        if (tl < 0 || o + (size_t)tl >= outcap)    return false;   // too many specifiers
        memcpy(out + o, tok, (size_t)tl);
        o += (size_t)tl; out[o] = '\0';
    }
    return true;
}

// Load English once as the trusted reference (README lists English.ini as the
// required default). Validation degrades gracefully if it is missing.
static void ensure_baseline(void) {
    if (lang_baseline_count > 0) return;
    char dir[MAX_PATH]; get_lang_dir(dir);
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s/English.ini", dir);
    load_ini_into(path, lang_baseline, &lang_baseline_count);
}

// Validate every loaded translation against the English baseline. A value is
// kept only if it is safe and its specifier signature matches English's for the
// same key; otherwise fall back to the English text, or — if even that is
// missing/unsafe — to the key name. Both fallbacks are literal-safe to format.
static void sanitize_against_baseline(void) {
    for (int i = 0; i < lang_entry_count; i++) {
        char sig_a[128];
        bool ok_a = fmt_signature(lang_table[i].val, sig_a, sizeof(sig_a));

        const char *base_val = NULL;
        char sig_b[128]; bool ok_b = false;
        for (int j = 0; j < lang_baseline_count; j++) {
            if (strcmp(lang_baseline[j].key, lang_table[i].key) == 0) {
                base_val = lang_baseline[j].val;
                ok_b = fmt_signature(base_val, sig_b, sizeof(sig_b));
                break;
            }
        }

        if (base_val) {
            if (ok_a && ok_b && strcmp(sig_a, sig_b) == 0) continue;   // matches English
            const char *repl = ok_b ? base_val : lang_table[i].key;
            vtree_log("[lang] '%s': format mismatch/unsafe — falling back to %s\n",
                      lang_table[i].key, ok_b ? "English" : "key");
            snprintf(lang_table[i].val, MAX_LANG_VAL_LEN, "%s", repl);
        } else if (!ok_a) {
            // No English reference for this key and the value is unsafe — neuter.
            vtree_log("[lang] '%s': unsafe, no baseline — using key\n", lang_table[i].key);
            snprintf(lang_table[i].val, MAX_LANG_VAL_LEN, "%s", lang_table[i].key);
        }
    }
}

// ---------------------------------------------------------------------------
// Parse one language INI file into the active table, then sanitize it against
// the English baseline so no untrusted format string reaches a printf call.
// ---------------------------------------------------------------------------
static void lang_load_file_path(const char *path) {
    ensure_baseline();
    load_ini_into(path, lang_table, &lang_entry_count);
    sanitize_against_baseline();
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
        memcpy(lang_names[lang_file_count], n, nlen);   // copy basename without ".ini"
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
