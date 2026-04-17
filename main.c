#include "vtree.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
#define VTREE_VERSION "0.7"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
AppConfig cfg;
AppState  panes[2];
int       active_pane = 0;
AppMode   current_mode = MODE_EXPLORER;
bool      debug_mode = false;
FILE     *debug_log_file = NULL;

// ---------------------------------------------------------------------------
// Logging — writes to stdout + optional log file, with a wall-clock timestamp.
// Uses SDL_GetTicks() (ms since SDL_Init); shows 0 for the two pre-init lines.
// Only emits output when debug_mode is true.
// ---------------------------------------------------------------------------
void vtree_log(const char *fmt, ...) {
    if (!debug_mode) return;
    Uint32 ms = SDL_GetTicks();
    char ts[20];
    snprintf(ts, sizeof(ts), "[%4u.%03u] ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));

    va_list ap;
    va_start(ap, fmt); fputs(ts, stdout); vprintf(fmt, ap); va_end(ap);
    if (debug_log_file) {
        va_start(ap, fmt);
        fputs(ts, debug_log_file); vfprintf(debug_log_file, fmt, ap);
        fflush(debug_log_file);
        va_end(ap);
    }
}
bool      delete_confirm_active  = false;
bool      paste_conflict_active  = false;
int       paste_conflict_sel     = 0;   // selected option in conflict modal
int       paste_conflict_count   = 0;   // how many clipboard items conflict
bool      paste_dest_active      = false;
int       paste_dest_sel         = 0;   // 0 = this pane, 1 = other pane
int       paste_dest_pane        = 0;   // resolved destination pane index
static bool do_symlink_after_dest = false;  // dest chooser is for symlink, not paste
int       settings_index = 0;
Clipboard clip = { .op = OP_NONE, .count = 0 };

GlyphEntry glyph_cache[GLYPH_CACHE_SIZE];
Uint32     glyph_frame = 0;

// Top-level menu
#define TOPMENU_FILES    0
#define TOPMENU_SETTINGS 1
#define TOPMENU_ABOUT    2
#define TOPMENU_EXIT     3
#define TOPMENU_MAX      4
static const char *topmenu_items[TOPMENU_MAX] = { "Files", "Settings", "About", "Exit" };

// File-ops submenu
#define FILEMENU_COPY    0
#define FILEMENU_CUT     1
#define FILEMENU_PASTE   2
#define FILEMENU_SYMLINK 3
#define FILEMENU_RENAME  4
#define FILEMENU_DELETE  5
#define FILEMENU_NEWFILE 6
#define FILEMENU_NEWDIR  7
#define FILEMENU_BACK    8
#define FILEMENU_MAX     9
static const char *filemenu_items[FILEMENU_MAX] = {
    "Copy", "Cut", "Paste", "Symlink", "Rename", "Delete", "New File", "New Folder", "Back"
};

SDL_Window        *window   = NULL;
SDL_Renderer      *renderer = NULL;
SDL_GameController *pad     = NULL;
static int phys_w = 0, phys_h = 0;        // physical display dims (set once at startup)
static SDL_Texture *render_target = NULL;  // off-screen target for rotation; NULL = no rotation
TTF_Font *font_list = NULL, *font_header = NULL, *font_footer = NULL, *font_menu = NULL, *font_hex = NULL;
SDL_Texture *tex_file = NULL, *tex_folder = NULL;
SDL_Texture *tex_img = NULL, *tex_txt = NULL, *tex_dirup = NULL;
SDL_Texture *tex_copy = NULL, *tex_cut = NULL, *tex_paste = NULL, *tex_symlink = NULL;
SDL_Texture *tex_rename = NULL, *tex_delete = NULL, *tex_settings = NULL, *tex_exit = NULL;
SDL_Texture *tex_newfile = NULL, *tex_newfolder = NULL, *tex_about = NULL;
SDL_Texture *tex_enterfol = NULL;
SDL_Texture *tex_viewer = NULL, *tex_hexview = NULL, *tex_imgview = NULL, *tex_fileinfo = NULL, *tex_exec = NULL;

// ---------------------------------------------------------------------------
// Font file list — populated by scan_fonts() at startup
// ---------------------------------------------------------------------------
#define MAX_FONT_FILES 64
static char font_files[MAX_FONT_FILES][MAX_PATH];
static int  font_file_count  = 0;
static int  current_font_idx = 0;

// Executable directory — resolved once, used for font path construction
char vtree_exe_dir[MAX_PATH] = "";

// Full resolved path of the currently active font — used by hexview auto-sizer
char vtree_font_path[MAX_PATH] = "";

static void init_exe_dir(void) {
    char buf[MAX_PATH];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        char *sl = strrchr(buf, '/');
        if (sl) { *sl = '\0'; strncpy(vtree_exe_dir, buf, MAX_PATH - 1); return; }
    }
    strncpy(vtree_exe_dir, ".", MAX_PATH - 1);
}

static bool has_font_ext(const char *name) {
    size_t nl = strlen(name);
    if (nl < 5) return false;
    const char *e = name + nl - 4;
    char lc[5];
    for (int i = 0; i < 4; i++) lc[i] = (char)tolower((unsigned char)e[i]);
    lc[4] = '\0';
    return strcmp(lc, ".ttf") == 0 || strcmp(lc, ".otf") == 0;
}

// Compare two font_files entries by basename for qsort
static int font_name_cmp(const void *a, const void *b) {
    const char *ba = strrchr((const char *)a, '/'); ba = ba ? ba + 1 : (const char *)a;
    const char *bb = strrchr((const char *)b, '/'); bb = bb ? bb + 1 : (const char *)b;
    return strcmp(ba, bb);
}

static void scan_fonts(void) {
    init_exe_dir();
    font_file_count = 0;

    // Scan fonts/ subdirectory next to the executable — the only font source
    char fonts_dir[MAX_PATH];
    snprintf(fonts_dir, MAX_PATH, "%s/fonts", vtree_exe_dir);
    DIR *d = opendir(fonts_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && font_file_count < MAX_FONT_FILES) {
            if (!has_font_ext(de->d_name)) continue;
            snprintf(font_files[font_file_count++], MAX_PATH, "%s/%s", fonts_dir, de->d_name);
        }
        closedir(d);
    }

    // Sort all entries by basename
    if (font_file_count > 1)
        qsort(font_files, font_file_count, MAX_PATH, font_name_cmp);

    // Match current_font_idx to cfg.font_path; if blank, use first available font
    current_font_idx = 0;
    if (cfg.font_path[0]) {
        const char *want = strrchr(cfg.font_path, '/');
        want = want ? want + 1 : cfg.font_path;
        for (int i = 0; i < font_file_count; i++) {
            const char *bn = strrchr(font_files[i], '/');
            bn = bn ? bn + 1 : font_files[i];
            if (strcmp(bn, want) == 0) { current_font_idx = i; break; }
        }
    }

    // Keep the resolved full path in sync for hexview auto-sizer
    if (font_file_count > 0)
        strncpy(vtree_font_path, font_files[current_font_idx], MAX_PATH - 1);
}


// ---------------------------------------------------------------------------
// File-type detection
// ---------------------------------------------------------------------------
static bool is_text_file(const char *name) {
    static const char *text_exts[] = {
        ".txt", ".md", ".cfg", ".ini", ".conf", ".log", ".lua", ".c", ".h",
        ".cpp", ".cc", ".py", ".sh", ".bash", ".json", ".xml", ".yaml", ".yml",
        ".csv", ".html", ".htm", ".css", ".js", ".ts", ".toml", ".rs", ".go",
        ".java", ".kt", ".swift", ".rb", ".pl", ".asm", ".s", ".nfo", ".me",
        ".readme", ".license", ".makefile", ".cmake", ".diff", ".patch",
        NULL
    };
    const char *dot = strrchr(name, '.');
    if (!dot) {
        const char *textnames[] = { "makefile", "readme", "license", "authors",
                                    "changelog", "copying", "install", NULL };
        char lower[256]; int i = 0;
        while (name[i] && i < 255) { lower[i] = (char)tolower((unsigned char)name[i]); i++; }
        lower[i] = '\0';
        for (int j = 0; textnames[j]; j++)
            if (strcmp(lower, textnames[j]) == 0) return true;
        return false;
    }
    char lower[32]; int i = 0;
    while (dot[i] && i < 31) { lower[i] = (char)tolower((unsigned char)dot[i]); i++; }
    lower[i] = '\0';
    for (int j = 0; text_exts[j]; j++)
        if (strcmp(lower, text_exts[j]) == 0) return true;
    // Check extra extensions from config
    for (int j = 0; j < cfg.extra_text_ext_count; j++)
        if (strcmp(lower, cfg.extra_text_exts[j]) == 0) return true;
    return false;
}

static bool is_image_file(const char *name) {
    static const char *img_exts[] = {
        ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga", ".tiff", ".tif", ".webp", ".lbm",
        ".pnm", ".pbm", ".pgm", ".ppm", ".xcf", ".xpm", ".svg",
        NULL
    };
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char lower[32]; int i = 0;
    while (dot[i] && i < 31) { lower[i] = (char)tolower((unsigned char)dot[i]); i++; }
    lower[i] = '\0';
    for (int j = 0; img_exts[j]; j++)
        if (strcmp(lower, img_exts[j]) == 0) return true;
    // Check extra extensions from config
    for (int j = 0; j < cfg.extra_image_ext_count; j++)
        if (strcmp(lower, cfg.extra_image_exts[j]) == 0) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Icon selection — returns the best matching texture for a file entry.
// Falls back gracefully: specific → tex_file → NULL.
// ---------------------------------------------------------------------------
static SDL_Texture *get_file_icon(const FileEntry *fe) {
    if (fe->is_dir) {
        if (strcmp(fe->name, "..") == 0)
            return tex_dirup ? tex_dirup : tex_folder;
        return tex_folder;
    }
    if (is_image_file(fe->name)) return tex_img ? tex_img : tex_file;
    if (is_text_file(fe->name))  return tex_txt ? tex_txt : tex_file;
    return tex_file;
}

// ---------------------------------------------------------------------------
// Action chooser — dynamic per file type
// ---------------------------------------------------------------------------
#define ACT_TEXT    0
#define ACT_HEX     1
#define ACT_IMG     2
#define ACT_INFO    3
#define ACT_CANCEL  4
#define ACT_EXEC    5
static const char *act_labels[] = { "Text Viewer", "Hex Viewer", "Image Viewer", "File Info", "Cancel", "Execute" };

static int  choose_actions[8];   // action IDs for current menu
static int  choose_count    = 0;
static int  choose_selection = 0;
static int  choose_default  = 0; // index of the auto-selected "best" viewer
static char choose_path[MAX_PATH];
static bool choose_path_is_dir = false;
static int  choose_marked   = 0; // marked-file count in active pane at open_file() time
static int  choose_pane     = 0; // active pane at open_file() time

// ---------------------------------------------------------------------------
// File info modal
// ---------------------------------------------------------------------------
#define FILEINFO_MAX_LINES 8
static bool fileinfo_active   = false;
static bool fileinfo_is_multi = false;
static bool fileinfo_is_dir   = false;
static bool exec_error_active = false;
static char exec_error_title[64] = "Cannot Execute";
static char exec_error_msg[256] = "";
static char fileinfo_lines[FILEINFO_MAX_LINES][256];
static int  fileinfo_line_count = 0;

static bool fs_supports_symlinks(const char *path) {
    struct statfs sfs;
    if (statfs(path, &sfs) != 0) return true;  // assume yes on error
    switch ((unsigned long)sfs.f_type) {
        case 0x4D44UL:       // MSDOS_SUPER_MAGIC  — FAT12/FAT16/FAT32/vFAT
        case 0x2011BAB0UL:   // EXFAT_SUPER_MAGIC  — exFAT (kernel driver)
        case 0x65735546UL:   // FUSEBLK_SUPER_MAGIC — FUSE block (fuse-exfat, NTFS-3g, etc.)
            return false;
        default:
            return true;
    }
}

static long long calc_dir_size(const char *path) {
    long long total = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char sub[MAX_PATH]; join_path(sub, path, e->d_name);
        struct stat st;
        if (lstat(sub, &st) == 0) {
            if (S_ISDIR(st.st_mode)) total += calc_dir_size(sub);
            else                     total += st.st_size;
        }
    }
    closedir(d);
    return total;
}

static void format_perms(mode_t m, char *out) {
    out[0] = S_ISLNK(m) ? 'l' : S_ISDIR(m) ? 'd' : '-';
    out[1] = (m & S_IRUSR) ? 'r' : '-'; out[2] = (m & S_IWUSR) ? 'w' : '-'; out[3] = (m & S_IXUSR) ? 'x' : '-';
    out[4] = (m & S_IRGRP) ? 'r' : '-'; out[5] = (m & S_IWGRP) ? 'w' : '-'; out[6] = (m & S_IXGRP) ? 'x' : '-';
    out[7] = (m & S_IROTH) ? 'r' : '-'; out[8] = (m & S_IWOTH) ? 'w' : '-'; out[9] = (m & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static void show_fileinfo(const char *path) {
    fileinfo_line_count = 0;
    struct stat lst, st;
    if (lstat(path, &lst) != 0) {
        snprintf(fileinfo_lines[fileinfo_line_count++], 256, "Cannot stat file");
        fileinfo_active = true;
        return;
    }
    bool is_link = S_ISLNK(lst.st_mode);
    struct stat *info = &lst;
    bool target_ok = false;
    if (is_link && stat(path, &st) == 0) { info = &st; target_ok = true; }

    // Name
    const char *base = strrchr(path, '/');
    snprintf(fileinfo_lines[fileinfo_line_count++], 256, "Name: %s", base ? base + 1 : path);

    // Size — for directories walk recursively, otherwise use stat size
    char sz[32];
    bool item_is_dir = S_ISDIR(info->st_mode);
    long long item_size = item_is_dir ? calc_dir_size(path) : (long long)info->st_size;
    format_size(item_size, sz);
    snprintf(fileinfo_lines[fileinfo_line_count++], 256, "Size: %s (%lld bytes)", sz, item_size);

    // Type
    const char *ftype = is_link ? (target_ok ? (S_ISDIR(info->st_mode) ? "Symlink -> Dir" : "Symlink -> File") : "Symlink (broken)") :
                        S_ISDIR(lst.st_mode) ? "Directory" : "Regular File";
    snprintf(fileinfo_lines[fileinfo_line_count++], 256, "Type: %s", ftype);

    // Permissions
    char perms[12];
    format_perms(lst.st_mode, perms);
    snprintf(fileinfo_lines[fileinfo_line_count++], 256, "Perms: %s  (octal %04o)", perms, (unsigned)(lst.st_mode & 07777));

    // Owner / group
    char owner[64] = "", group[64] = "";
    struct passwd *pw = getpwuid(lst.st_uid);
    struct group  *gr = getgrgid(lst.st_gid);
    if (pw) snprintf(owner, sizeof(owner), "%s", pw->pw_name);
    else    snprintf(owner, sizeof(owner), "%u", lst.st_uid);
    if (gr) snprintf(group, sizeof(group), "%s", gr->gr_name);
    else    snprintf(group, sizeof(group), "%u", lst.st_gid);
    snprintf(fileinfo_lines[fileinfo_line_count++], 256, "Owner: %s / %s", owner, group);

    // Modified time
    char tbuf[64];
    struct tm *tm = localtime(&lst.st_mtime);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d  %H:%M:%S", tm);
    snprintf(fileinfo_lines[fileinfo_line_count++], 256, "Modified: %s", tbuf);

    // Symlink target
    if (is_link) {
        char target[MAX_PATH] = "";
        ssize_t n = readlink(path, target, MAX_PATH - 1);
        if (n > 0) { target[n] = '\0'; snprintf(fileinfo_lines[fileinfo_line_count++], 256, "Target: %s", target); }
    }

    fileinfo_is_multi = false;
    fileinfo_is_dir   = item_is_dir;
    fileinfo_active   = true;
}

static void show_fileinfo_multi(void) {
    fileinfo_line_count = 0;
    AppState *s = &panes[choose_pane];

    int total = 0, nfiles = 0, ndirs = 0, nlinks = 0;
    long long total_size = 0;
    for (int i = 0; i < s->file_count; i++) {
        if (!s->files[i].marked) continue;
        total++;
        if      (s->files[i].is_dir)  ndirs++;
        else if (s->files[i].is_link) nlinks++;
        else                          nfiles++;
        char fp[MAX_PATH]; join_path(fp, s->current_path, s->files[i].name);
        total_size += s->files[i].is_dir ? calc_dir_size(fp) : s->files[i].size;
    }

    snprintf(fileinfo_lines[fileinfo_line_count++], 256, "%d item%s selected",
             total, total == 1 ? "" : "s");

    if (ndirs > 0 && nfiles > 0)
        snprintf(fileinfo_lines[fileinfo_line_count++], 256, "Files: %d   Directories: %d", nfiles, ndirs);
    else if (ndirs > 0)
        snprintf(fileinfo_lines[fileinfo_line_count++], 256, "Directories: %d", ndirs);
    else
        snprintf(fileinfo_lines[fileinfo_line_count++], 256, "Files: %d", nfiles);

    if (nlinks > 0)
        snprintf(fileinfo_lines[fileinfo_line_count++], 256, "Symlinks: %d", nlinks);

    char sz[32];
    format_size(total_size, sz);
    snprintf(fileinfo_lines[fileinfo_line_count++], 256, "Total size: %s", sz);

    fileinfo_is_multi = true;
    fileinfo_is_dir   = (ndirs > 0 && nfiles == 0 && nlinks == 0);
    fileinfo_active   = true;
}

// ---------------------------------------------------------------------------
// Settings page — tabbed layout
// ---------------------------------------------------------------------------
typedef enum { STYPE_PRESET, STYPE_FONT, STYPE_INT, STYPE_ACTION,
               STYPE_BOOL, STYPE_KEYBIND, STYPE_PATH, STYPE_CYCLE } SettingType;
typedef struct {
    const char *label;
    SettingType type;
    int  *int_ptr;
    bool *bool_ptr;
    SDL_GameControllerButton *btn_ptr;
    char *str_ptr;
    int   step, lo, hi;
    const char **opts;  // STYPE_CYCLE: option label array; hi = count
} SettingDef;

#define SETTINGS_TAB_COUNT 3
static const char *settings_tab_labels[SETTINGS_TAB_COUNT] = { "General", "Display", "Keys" };
static int settings_tab         = 0;
static int settings_tab_indices[SETTINGS_TAB_COUNT] = { 0, 0, 0 };

static const char *rotation_opts[] = { "None", "90 CW", "180", "270 CW" };
static SettingDef general_defs[] = {
    { "Show Hidden Files",    STYPE_BOOL,  NULL, &cfg.show_hidden,   NULL, NULL,           0, 0, 0, NULL },
    { "Remember Directories", STYPE_BOOL,  NULL, &cfg.remember_dirs, NULL, NULL,           0, 0, 0, NULL },
    { "Execute Scripts",      STYPE_BOOL,  NULL, &cfg.exec_scripts,  NULL, NULL,           0, 0, 0, NULL },
    { "Single Pane Mode",     STYPE_BOOL,  NULL, &cfg.single_pane,   NULL, NULL,           0, 0, 0, NULL },
    { "Rotation",             STYPE_CYCLE, &cfg.rotation, NULL, NULL, NULL,                0, 0, 4, rotation_opts },
    { "Start Dir: Left",      STYPE_PATH,  NULL, NULL, NULL, cfg.start_left,               0, 0, 0, NULL },
    { "Start Dir: Right",     STYPE_PATH,  NULL, NULL, NULL, cfg.start_right,              0, 0, 0, NULL },
    { "Save Config",          STYPE_ACTION,NULL, NULL, NULL, NULL,                         0, 0, 0, NULL },
    { "Close",                STYPE_ACTION,NULL, NULL, NULL, NULL,                         0, 0, 0, NULL },
};
static SettingDef display_defs[] = {
    { "Theme Preset",  STYPE_PRESET, NULL,                  NULL, NULL, NULL, 0,  0,    0   },
    { "Font File",     STYPE_FONT,   NULL,                  NULL, NULL, NULL, 0,  0,    0   },
    { "Screen Width",  STYPE_INT,    &cfg.screen_w,         NULL, NULL, NULL, 16, 320,  1920 },
    { "Screen Height", STYPE_INT,    &cfg.screen_h,         NULL, NULL, NULL, 16, 240,  1080 },
    { "Font: List",    STYPE_INT,    &cfg.font_size_list,   NULL, NULL, NULL, 1,  8,    48   },
    { "Font: Header",  STYPE_INT,    &cfg.font_size_header, NULL, NULL, NULL, 1,  8,    48   },
    { "Font: Footer",  STYPE_INT,    &cfg.font_size_footer, NULL, NULL, NULL, 1,  8,    48   },
    { "Font: Menu",    STYPE_INT,    &cfg.font_size_menu,   NULL, NULL, NULL, 1,  8,    48   },
    { "Save Config",   STYPE_ACTION, NULL,                  NULL, NULL, NULL, 0,  0,    0    },
    { "Close",         STYPE_ACTION, NULL,                  NULL, NULL, NULL, 0,  0,    0    },
};
// Pending key bindings — edited in-session, only written to cfg on explicit Save.
// Prevents live rebinding from locking the user out of settings mid-session.
typedef struct {
    SDL_GameControllerButton k_confirm, k_back, k_menu, k_mark;
    SDL_GameControllerButton k_pgup, k_pgdn;
    SDL_GameControllerButton osk_k_type, osk_k_bksp, osk_k_shift;
    SDL_GameControllerButton osk_k_cancel, osk_k_toggle, osk_k_ins;
} PendingKeys;
static PendingKeys pending_keys;

// Per-group pointer arrays for duplicate detection.
// General and OSK keys are checked independently — cross-group sharing is fine
// (e.g. Confirm and OSK: Type Key can both be A).
static SDL_GameControllerButton *pending_general_ptrs[] = {
    &pending_keys.k_confirm, &pending_keys.k_back,
    &pending_keys.k_menu,    &pending_keys.k_mark,
    &pending_keys.k_pgup,    &pending_keys.k_pgdn,
};
static SDL_GameControllerButton *pending_osk_ptrs[] = {
    &pending_keys.osk_k_type,   &pending_keys.osk_k_bksp,
    &pending_keys.osk_k_shift,  &pending_keys.osk_k_cancel,
    &pending_keys.osk_k_toggle, &pending_keys.osk_k_ins,
};
#define PENDING_GENERAL_COUNT ((int)(sizeof(pending_general_ptrs)/sizeof(pending_general_ptrs[0])))
#define PENDING_OSK_COUNT     ((int)(sizeof(pending_osk_ptrs)/sizeof(pending_osk_ptrs[0])))

// Returns the group array + count for a given btn_ptr, or NULL if not found.
static SDL_GameControllerButton **pending_group_for(SDL_GameControllerButton *target, int *count) {
    for (int i = 0; i < PENDING_GENERAL_COUNT; i++)
        if (pending_general_ptrs[i] == target) { *count = PENDING_GENERAL_COUNT; return pending_general_ptrs; }
    for (int i = 0; i < PENDING_OSK_COUNT; i++)
        if (pending_osk_ptrs[i] == target)     { *count = PENDING_OSK_COUNT;     return pending_osk_ptrs; }
    *count = 0; return NULL;
}

static void pending_keys_from_cfg(void) {
    pending_keys.k_confirm    = cfg.k_confirm;
    pending_keys.k_back       = cfg.k_back;
    pending_keys.k_menu       = cfg.k_menu;
    pending_keys.k_mark       = cfg.k_mark;
    pending_keys.k_pgup       = cfg.k_pgup;
    pending_keys.k_pgdn       = cfg.k_pgdn;
    pending_keys.osk_k_type   = cfg.osk_k_type;
    pending_keys.osk_k_bksp   = cfg.osk_k_bksp;
    pending_keys.osk_k_shift  = cfg.osk_k_shift;
    pending_keys.osk_k_cancel = cfg.osk_k_cancel;
    pending_keys.osk_k_toggle = cfg.osk_k_toggle;
    pending_keys.osk_k_ins    = cfg.osk_k_ins;
}
static void pending_keys_to_cfg(void) {
    cfg.k_confirm    = pending_keys.k_confirm;
    cfg.k_back       = pending_keys.k_back;
    cfg.k_menu       = pending_keys.k_menu;
    cfg.k_mark       = pending_keys.k_mark;
    cfg.k_pgup       = pending_keys.k_pgup;
    cfg.k_pgdn       = pending_keys.k_pgdn;
    cfg.osk_k_type   = pending_keys.osk_k_type;
    cfg.osk_k_bksp   = pending_keys.osk_k_bksp;
    cfg.osk_k_shift  = pending_keys.osk_k_shift;
    cfg.osk_k_cancel = pending_keys.osk_k_cancel;
    cfg.osk_k_toggle = pending_keys.osk_k_toggle;
    cfg.osk_k_ins    = pending_keys.osk_k_ins;
}

static SettingDef keys_defs[] = {
    { "Confirm",          STYPE_KEYBIND, NULL, NULL, &pending_keys.k_confirm,    NULL, 0, 0, 0 },
    { "Back / Up Dir",    STYPE_KEYBIND, NULL, NULL, &pending_keys.k_back,       NULL, 0, 0, 0 },
    { "Menu",             STYPE_KEYBIND, NULL, NULL, &pending_keys.k_menu,       NULL, 0, 0, 0 },
    { "Mark File",        STYPE_KEYBIND, NULL, NULL, &pending_keys.k_mark,       NULL, 0, 0, 0 },
    { "Page Up",          STYPE_KEYBIND, NULL, NULL, &pending_keys.k_pgup,       NULL, 0, 0, 0 },
    { "Page Down",        STYPE_KEYBIND, NULL, NULL, &pending_keys.k_pgdn,       NULL, 0, 0, 0 },
    { "OSK: Type Key",    STYPE_KEYBIND, NULL, NULL, &pending_keys.osk_k_type,   NULL, 0, 0, 0 },
    { "OSK: Backspace",   STYPE_KEYBIND, NULL, NULL, &pending_keys.osk_k_bksp,   NULL, 0, 0, 0 },
    { "OSK: Shift Layer", STYPE_KEYBIND, NULL, NULL, &pending_keys.osk_k_shift,  NULL, 0, 0, 0 },
    { "OSK: Cancel",      STYPE_KEYBIND, NULL, NULL, &pending_keys.osk_k_cancel, NULL, 0, 0, 0 },
    { "OSK: Toggle KB",   STYPE_KEYBIND, NULL, NULL, &pending_keys.osk_k_toggle, NULL, 0, 0, 0 },
    { "OSK: Ins/Ovr",     STYPE_KEYBIND, NULL, NULL, &pending_keys.osk_k_ins,    NULL, 0, 0, 0 },
    { "Save Config",      STYPE_ACTION,  NULL, NULL, NULL,                       NULL, 0, 0, 0 },
    { "Close",            STYPE_ACTION,  NULL, NULL, NULL,                       NULL, 0, 0, 0 },
};

static SettingDef *tab_defs(int *count) {
    switch (settings_tab) {
        case 0: *count = (int)(sizeof(general_defs)/sizeof(general_defs[0])); return general_defs;
        case 1: *count = (int)(sizeof(display_defs)/sizeof(display_defs[0])); return display_defs;
        case 2: *count = (int)(sizeof(keys_defs)/sizeof(keys_defs[0]));       return keys_defs;
        default: *count = 0; return NULL;
    }
}

static bool   settings_dirty       = false;  // unsaved changes pending
static bool   settings_save_prompt = false;  // "save before close?" modal active
static int    save_prompt_sel      = 0;      // 0=Save, 1=Discard
static Uint32 settings_save_toast  = 0;      // non-zero until toast expires
static int    settings_toast_tw    = 0;      // cached toast text width (set when toast activates)
static char   settings_toast_msg[64] = "Config saved.";
static AppConfig cfg_snapshot;               // cfg state at settings open (for discard)
static int    snapshot_theme_idx   = -1;     // current_named_theme at settings open
static int    snapshot_font_idx    = 0;      // current_font_idx at settings open
static bool   settings_listening   = false;  // waiting for next button press to bind
static SDL_GameControllerButton *settings_listen_target = NULL;

// ---------------------------------------------------------------------------
// Glyph cache
// ---------------------------------------------------------------------------
void destroy_glyph_cache() {
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (glyph_cache[i].texture) { SDL_DestroyTexture(glyph_cache[i].texture); glyph_cache[i].texture = NULL; }
    }
}

void draw_txt(TTF_Font *f, const char *txt, int x, int y, SDL_Color col) {
    if (!txt || !txt[0]) return;
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        GlyphEntry *e = &glyph_cache[i];
        if (!e->texture) continue;
        if (e->font == f && e->color.r == col.r && e->color.g == col.g &&
            e->color.b == col.b && e->color.a == col.a &&
            strncmp(e->text, txt, MAX_PATH - 1) == 0) {
            e->last_used = glyph_frame;
            SDL_Rect r = { x, y, e->w, e->h }; SDL_RenderCopy(renderer, e->texture, NULL, &r);
            return;
        }
    }
    SDL_Surface *surf = TTF_RenderText_Blended(f, txt, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    int slot = 0; Uint32 oldest = UINT32_MAX;
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (!glyph_cache[i].texture) { slot = i; oldest = 0; break; }
        if (glyph_cache[i].last_used < oldest) { oldest = glyph_cache[i].last_used; slot = i; }
    }
    if (glyph_cache[slot].texture) SDL_DestroyTexture(glyph_cache[slot].texture);
    strncpy(glyph_cache[slot].text, txt, MAX_PATH - 1); glyph_cache[slot].text[MAX_PATH - 1] = '\0';
    glyph_cache[slot].font = f; glyph_cache[slot].color = col; glyph_cache[slot].texture = tex;
    glyph_cache[slot].w = surf->w; glyph_cache[slot].h = surf->h; glyph_cache[slot].last_used = glyph_frame;
    SDL_Rect r = { x, y, surf->w, surf->h }; SDL_RenderCopy(renderer, tex, NULL, &r);
    SDL_FreeSurface(surf);
}

// ---------------------------------------------------------------------------
// Font reload
// ---------------------------------------------------------------------------
static void reload_fonts() {
    if (font_file_count == 0) return;
    const char *fp = font_files[current_font_idx];
    if (font_list)   { TTF_CloseFont(font_list);   font_list   = TTF_OpenFont(fp, cfg.font_size_list);   }
    if (font_header) { TTF_CloseFont(font_header); font_header = TTF_OpenFont(fp, cfg.font_size_header); }
    if (font_footer) { TTF_CloseFont(font_footer); font_footer = TTF_OpenFont(fp, cfg.font_size_footer); }
    if (font_menu)   { TTF_CloseFont(font_menu);   font_menu   = TTF_OpenFont(fp, cfg.font_size_menu);   }
    if (font_hex)    { TTF_CloseFont(font_hex);    font_hex    = TTF_OpenFont(fp, cfg.font_size_hex);    }
    destroy_glyph_cache();
    vtree_log("Fonts reloaded: %s  list=%d header=%d footer=%d menu=%d hex=%d\n",
              fp, cfg.font_size_list, cfg.font_size_header,
              cfg.font_size_footer, cfg.font_size_menu, cfg.font_size_hex);
}

// ---------------------------------------------------------------------------
// Settings helpers + render
// ---------------------------------------------------------------------------
static void settings_adjust(int dir) {
    int n; SettingDef *defs = tab_defs(&n);
    if (!defs || settings_index >= n) return;
    SettingDef *d = &defs[settings_index];
    if (d->type == STYPE_INT && d->int_ptr) {
        int old = *d->int_ptr;
        *d->int_ptr = SDL_clamp(*d->int_ptr + dir * d->step, d->lo, d->hi);
        vtree_log("Setting '%s': %d -> %d\n", d->label, old, *d->int_ptr);
        settings_dirty = true;
    } else if (d->type == STYPE_BOOL && d->bool_ptr) {
        *d->bool_ptr = !(*d->bool_ptr);
        vtree_log("Setting '%s': %s\n", d->label, *d->bool_ptr ? "true" : "false");
        settings_dirty = true;
        if (d->bool_ptr == &cfg.show_hidden) {
            load_dir(0, panes[0].current_path);
            load_dir(1, panes[1].current_path);
        } else if (d->bool_ptr == &cfg.exec_scripts && cfg.exec_scripts) {
            strncpy(settings_toast_msg, "Experimental: scripts run as current user.",
                    sizeof(settings_toast_msg) - 1);
            settings_save_toast = SDL_GetTicks() + 3500;
            settings_toast_tw   = 0;
            if (font_menu) TTF_SizeText(font_menu, settings_toast_msg, &settings_toast_tw, NULL);
        }
    } else if (d->type == STYPE_CYCLE && d->int_ptr && d->opts) {
        int count = d->hi;
        *d->int_ptr = ((*d->int_ptr) + dir + count) % count;
        vtree_log("Setting '%s': %s\n", d->label, d->opts[*d->int_ptr]);
        settings_dirty = true;
    } else if (d->type == STYPE_PRESET) {
        int n = named_theme_count;
        if (n < 1) return;
        int next = (current_named_theme < 0)
            ? ((dir > 0) ? 0 : n - 1)
            : (current_named_theme + dir + n) % n;
        vtree_log("Theme: %s -> %s\n",
                  current_named_theme >= 0 ? named_themes[current_named_theme].name : "(none)",
                  named_themes[next].name);
        apply_theme_preset(next); destroy_glyph_cache();
        settings_dirty = true;
    } else if (d->type == STYPE_FONT) {
        if (font_file_count < 1) return;
        current_font_idx = (current_font_idx + dir + font_file_count) % font_file_count;
        const char *bn = strrchr(font_files[current_font_idx], '/');
        bn = bn ? bn + 1 : font_files[current_font_idx];
        vtree_log("Font file: %s -> %s\n", cfg.font_path, bn);
        strncpy(cfg.font_path, bn, MAX_PATH - 1);
        strncpy(vtree_font_path, font_files[current_font_idx], MAX_PATH - 1);
        reload_fonts();
        settings_dirty = true;
    }
}
static void settings_do_close() {
    if (settings_dirty) {
        // Discard path: restore cfg and UI state to what they were when settings opened
        cfg                 = cfg_snapshot;
        current_named_theme = snapshot_theme_idx;
        current_font_idx    = snapshot_font_idx;
        if (font_file_count > 0 && current_font_idx < font_file_count)
            strncpy(vtree_font_path, font_files[current_font_idx], MAX_PATH - 1);
        destroy_glyph_cache();
    }
    reload_fonts();

    // Recompute logical dims from physical and (possibly changed) rotation
    cfg.screen_w = (cfg.rotation == 1 || cfg.rotation == 3) ? phys_h : phys_w;
    cfg.screen_h = (cfg.rotation == 1 || cfg.rotation == 3) ? phys_w : phys_h;

    // Recreate render target for the new rotation
    if (render_target) { SDL_DestroyTexture(render_target); render_target = NULL; }
    if (cfg.rotation != 0) {
        render_target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_TARGET,
                                          cfg.screen_w, cfg.screen_h);
        if (render_target) SDL_SetRenderTarget(renderer, render_target);
    } else {
        SDL_SetRenderTarget(renderer, NULL);
    }

    SDL_SetWindowSize(window, phys_w, phys_h);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    settings_dirty         = false;
    settings_save_prompt   = false;
    settings_listening     = false;
    settings_listen_target = NULL;
    current_mode           = MODE_EXPLORER;
}
static void settings_confirm() {
    int n; SettingDef *defs = tab_defs(&n);
    if (!defs || settings_index >= n) return;
    SettingDef *d = &defs[settings_index];

    if (d->type == STYPE_ACTION) {
        if (settings_index == n - 2) {  // Save Config
            pending_keys_to_cfg();
            save_config();
            cfg_snapshot       = cfg;
            snapshot_theme_idx = current_named_theme;
            snapshot_font_idx  = current_font_idx;
            settings_dirty      = false;
            settings_save_toast = SDL_GetTicks() + 1800;
            settings_toast_tw   = 0;
            strncpy(settings_toast_msg, "Config saved.", sizeof(settings_toast_msg) - 1);
            if (font_menu) TTF_SizeText(font_menu, settings_toast_msg, &settings_toast_tw, NULL);
        } else if (settings_index == n - 1) {  // Close
            if (settings_dirty) { settings_save_prompt = true; save_prompt_sel = 0; }
            else                  settings_do_close();
        }
    } else if (d->type == STYPE_BOOL && d->bool_ptr) {
        settings_adjust(1);  // toggle
    } else if (d->type == STYPE_CYCLE) {
        settings_adjust(1);  // cycle forward
    } else if (d->type == STYPE_KEYBIND && d->btn_ptr) {
        settings_listening     = true;
        settings_listen_target = d->btn_ptr;
    } else if (d->type == STYPE_PATH && d->str_ptr) {
        if (!cfg.remember_dirs)
            osk_enter_path(d->str_ptr, d->str_ptr);
    }
}
static void settings_try_close() {   // called from B/back button
    if (settings_dirty) { settings_save_prompt = true; save_prompt_sel = 0; }
    else                  settings_do_close();
}
static void draw_settings() {
    SDL_SetRenderDrawColor(renderer, cfg.theme.bg.r, cfg.theme.bg.g, cfg.theme.bg.b, 255);
    SDL_RenderClear(renderer);

    int hh  = cfg.font_size_header + 12;
    int th  = cfg.font_size_footer + 10;   // tab strip height
    int ih  = cfg.font_size_menu + 10;
    int cl  = 20;
    int cv  = cfg.screen_w / 2 + 20;
    int aw  = cfg.font_size_menu;
    int fh  = cfg.font_size_footer + 16;
    int rows_y0 = hh + th;                 // rows start below header + tab strip

    // Header bar
    SDL_SetRenderDrawColor(renderer, cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
    SDL_Rect hr = {0, 0, cfg.screen_w, hh}; SDL_RenderFillRect(renderer, &hr);
    draw_txt(font_header, "SETTINGS", cl, 5, cfg.theme.text);

    // Tab strip
    int tab_w = cfg.screen_w / SETTINGS_TAB_COUNT;
    for (int t = 0; t < SETTINGS_TAB_COUNT; t++) {
        SDL_Rect tr = {t * tab_w, hh, tab_w, th};
        if (t == settings_tab) {
            SDL_SetRenderDrawColor(renderer, cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g, cfg.theme.highlight_bg.b, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, cfg.theme.alt_bg.r, cfg.theme.alt_bg.g, cfg.theme.alt_bg.b, 255);
        }
        SDL_RenderFillRect(renderer, &tr);
        SDL_Color tc = (t == settings_tab) ? cfg.theme.marked : cfg.theme.text_disabled;
        int tw_px = 0;
        if (font_footer) TTF_SizeText(font_footer, settings_tab_labels[t], &tw_px, NULL);
        int tx = t * tab_w + (tab_w - tw_px) / 2;
        draw_txt(font_footer, settings_tab_labels[t], tx, hh + (th - cfg.font_size_footer) / 2, tc);
    }
    // Tab strip bottom border
    SDL_SetRenderDrawColor(renderer, cfg.theme.text_disabled.r, cfg.theme.text_disabled.g, cfg.theme.text_disabled.b, 255);
    SDL_RenderDrawLine(renderer, 0, hh + th - 1, cfg.screen_w, hh + th - 1);

    // Settings rows — scrolled so the selected item is always visible
    int n; SettingDef *defs = tab_defs(&n);
    int max_rows = (cfg.screen_h - rows_y0 - fh) / ih;
    if (max_rows < 1) max_rows = 1;
    int scroll = 0;
    if (settings_index >= scroll + max_rows) scroll = settings_index - max_rows + 1;
    if (scroll > n - max_rows)              scroll = SDL_max(0, n - max_rows);
    SDL_Rect row_clip = {0, rows_y0, cfg.screen_w, cfg.screen_h - rows_y0 - fh};
    SDL_RenderSetClipRect(renderer, &row_clip);
    for (int i = scroll; i < n && i < scroll + max_rows; i++) {
        SettingDef *d = &defs[i];
        int y = rows_y0 + 8 + (i - scroll) * ih;

        // Row background
        if (i == settings_index) {
            SDL_SetRenderDrawColor(renderer, cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g, cfg.theme.highlight_bg.b, 255);
            SDL_Rect row = {0, y-2, cfg.screen_w, ih}; SDL_RenderFillRect(renderer, &row);
        } else if (i % 2 != 0) {
            SDL_SetRenderDrawColor(renderer, cfg.theme.alt_bg.r, cfg.theme.alt_bg.g, cfg.theme.alt_bg.b, 255);
            SDL_Rect row = {0, y-2, cfg.screen_w, ih}; SDL_RenderFillRect(renderer, &row);
        }

        // Greyed label for path rows when RememberDirs is on
        bool greyed = (d->type == STYPE_PATH && cfg.remember_dirs);
        SDL_Color lc = greyed ? cfg.theme.text_disabled
                              : (i == settings_index) ? cfg.theme.marked : cfg.theme.text;
        draw_txt(font_menu, d->label, cl, y, lc);

        // Value column
        char val[80] = ""; bool arrows = false;
        SDL_Color vc = lc;

        if (d->type == STYPE_INT && d->int_ptr) {
            snprintf(val, sizeof(val), "%d", *d->int_ptr);
            arrows = true;
        } else if (d->type == STYPE_BOOL && d->bool_ptr) {
            snprintf(val, sizeof(val), "%s", *d->bool_ptr ? "Yes" : "No");
            arrows = true;
        } else if (d->type == STYPE_PRESET) {
            if (current_named_theme >= 0 && current_named_theme < named_theme_count)
                snprintf(val, sizeof(val), "%s", named_themes[current_named_theme].name);
            else
                snprintf(val, sizeof(val), "Custom");
            arrows = true;
        } else if (d->type == STYPE_FONT) {
            if (font_file_count > 0 && current_font_idx < font_file_count) {
                const char *bn = strrchr(font_files[current_font_idx], '/');
                snprintf(val, sizeof(val), "%s", bn ? bn + 1 : font_files[current_font_idx]);
            } else {
                snprintf(val, sizeof(val), "%s", cfg.font_path);
            }
            arrows = true;
        } else if (d->type == STYPE_CYCLE && d->int_ptr && d->opts) {
            int idx = *d->int_ptr;
            if (idx >= 0 && idx < d->hi)
                snprintf(val, sizeof(val), "%s", d->opts[idx]);
            arrows = true;
        } else if (d->type == STYPE_KEYBIND && d->btn_ptr) {
            if (settings_listening && settings_listen_target == d->btn_ptr) {
                snprintf(val, sizeof(val), "Press button...");
                vc = cfg.theme.marked;
            } else {
                snprintf(val, sizeof(val), "%s", btn_label(*d->btn_ptr));
            }
        } else if (d->type == STYPE_PATH && d->str_ptr) {
            // Show last 28 chars of path, prefixed with "..." if truncated
            const char *p = d->str_ptr;
            int plen = (int)strlen(p);
            if (plen > 28) { snprintf(val, sizeof(val), "...%s", p + (plen - 28)); }
            else            { snprintf(val, sizeof(val), "%s", p); }
            vc = greyed ? cfg.theme.text_disabled
                        : (i == settings_index) ? cfg.theme.marked : cfg.theme.text;
        } else {
            // STYPE_ACTION
            snprintf(val, sizeof(val), "[ %s ]", d->label);
            vc = (i == settings_index) ? cfg.theme.marked : cfg.theme.text_disabled;
        }

        if (arrows && i == settings_index) {
            draw_txt(font_menu, "<", cv, y, vc);
            draw_txt(font_menu, val, cv + aw + 6, y, vc);
            draw_txt(font_menu, ">", cv + aw + 6 + (int)strlen(val) * (aw - 2) + 6, y, vc);
        } else {
            draw_txt(font_menu, val, cv, y, vc);
        }
    }

    SDL_RenderSetClipRect(renderer, NULL);

    // Centre divider
    SDL_SetRenderDrawColor(renderer, cfg.theme.text_disabled.r, cfg.theme.text_disabled.g, cfg.theme.text_disabled.b, 255);
    SDL_RenderDrawLine(renderer, cfg.screen_w / 2, hh + th, cfg.screen_w / 2, cfg.screen_h);

    // Footer
    SDL_SetRenderDrawColor(renderer, cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
    SDL_Rect fr = {0, cfg.screen_h - fh, cfg.screen_w, fh}; SDL_RenderFillRect(renderer, &fr);
    const char *hint = settings_listening
        ? "Press any button to bind   B: Cancel"
        : "Up/Dn: Navigate   L/R: Change   A: Confirm   L1/R1: Switch tab   B: Close";
    draw_txt(font_footer, hint, 12, cfg.screen_h - fh + 6, cfg.theme.text_disabled);

    // Save prompt modal
    if (settings_save_prompt) {
        int hh  = cfg.font_size_footer + 14;
        int spc = cfg.font_size_menu + 14;
        int mw  = cfg.screen_w - 100;
        int mh  = hh + 2 * spc + 8;
        int mx  = (cfg.screen_w - mw) / 2, my = (cfg.screen_h - mh) / 2;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
        SDL_Rect scr = {0,0,cfg.screen_w,cfg.screen_h}; SDL_RenderFillRect(renderer, &scr);
        SDL_Rect mbg = {mx,my,mw,mh};
        SDL_Rect bdy = {mx,my+hh,mw,mh-hh};
        SDL_SetRenderDrawColor(renderer, cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
        SDL_RenderFillRect(renderer, &mbg);
        SDL_SetRenderDrawColor(renderer, cfg.theme.menu_bg.r, cfg.theme.menu_bg.g, cfg.theme.menu_bg.b, cfg.theme.menu_bg.a);
        SDL_RenderFillRect(renderer, &bdy);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer, cfg.theme.menu_border.r, cfg.theme.menu_border.g, cfg.theme.menu_border.b, 255);
        SDL_RenderDrawRect(renderer, &mbg);
        SDL_RenderSetClipRect(renderer, &mbg);
        draw_txt(font_footer, "Unsaved changes - save before closing?", mx + 12,
                 my + (hh - cfg.font_size_footer) / 2, cfg.theme.text_disabled);
        const char *opts[2] = { "Save and Close", "Discard and Close" };
        for (int i = 0; i < 2; i++) {
            int iy = my + hh + i * spc;
            if (i == save_prompt_sel) {
                SDL_SetRenderDrawColor(renderer, cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g, cfg.theme.highlight_bg.b, 255);
                SDL_Rect row = {mx+2, iy-2, mw-4, spc}; SDL_RenderFillRect(renderer, &row);
            }
            SDL_Color lc = (i == save_prompt_sel) ? cfg.theme.marked : cfg.theme.text;
            draw_txt(font_menu, opts[i], mx + 16, iy + (spc - cfg.font_size_menu) / 2, lc);
        }
        SDL_RenderSetClipRect(renderer, NULL);
    }

    // Toast (Config saved / Already bound / etc.)
    if (settings_save_toast && SDL_GetTicks() < settings_save_toast) {
        const char *msg = settings_toast_msg;
        int tw = settings_toast_tw;
        int th = cfg.font_size_menu + 16;
        int tx = (cfg.screen_w - tw - 20) / 2, ty = cfg.screen_h - fh - th - 8;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g, cfg.theme.highlight_bg.b, 220);
        SDL_Rect tr = {tx, ty, tw + 20, th}; SDL_RenderFillRect(renderer, &tr);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        draw_txt(font_menu, msg, tx + 10, ty + (th - cfg.font_size_menu) / 2, cfg.theme.marked);
    }
}


// ---------------------------------------------------------------------------
// Action chooser — modal overlay
// ---------------------------------------------------------------------------
static void draw_open_chooser() {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
    SDL_Rect scr = {0, 0, cfg.screen_w, cfg.screen_h};
    SDL_RenderFillRect(renderer, &scr);

    int isz = 32;
    int spc = (cfg.font_size_menu + 14 > isz + 8) ? cfg.font_size_menu + 14 : isz + 8;
    int hh  = cfg.font_size_footer + 14;
    int mw  = cfg.screen_w - 80;
    int mh  = hh + choose_count * spc + 8;
    int mx  = (cfg.screen_w - mw) / 2;
    int my  = (cfg.screen_h - mh) / 2;

    SDL_Rect mbg  = {mx, my, mw, mh};
    SDL_Rect body = {mx, my + hh, mw, mh - hh};
    SDL_SetRenderDrawColor(renderer, cfg.theme.header_bg.r, cfg.theme.header_bg.g,
                           cfg.theme.header_bg.b, 255);
    SDL_RenderFillRect(renderer, &mbg);
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_bg.r, cfg.theme.menu_bg.g,
                           cfg.theme.menu_bg.b, cfg.theme.menu_bg.a);
    SDL_RenderFillRect(renderer, &body);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_border.r, cfg.theme.menu_border.g,
                           cfg.theme.menu_border.b, 255);
    SDL_RenderDrawRect(renderer, &mbg);

    SDL_RenderSetClipRect(renderer, &mbg);
    draw_txt(font_footer, "Action:", mx + 12, my + (hh - cfg.font_size_footer) / 2, cfg.theme.text_disabled);

    SDL_Texture *act_icons[] = { tex_viewer, tex_hexview, tex_imgview, tex_fileinfo, NULL, tex_exec };

    for (int i = 0; i < choose_count; i++) {
        int act = choose_actions[i];
        int iy  = my + hh + i * spc;
        if (i == choose_selection) {
            SDL_SetRenderDrawColor(renderer, cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g,
                                   cfg.theme.highlight_bg.b, 255);
            SDL_Rect row = {mx + 2, iy - 2, mw - 4, spc};
            SDL_RenderFillRect(renderer, &row);
        }
        SDL_Texture *icn = (act < 6) ? act_icons[act] : NULL;
        if (icn) { SDL_Rect ir = {mx + 12, iy + (spc - isz) / 2, isz, isz}; SDL_SetTextureAlphaMod(icn, 255); SDL_RenderCopy(renderer, icn, NULL, &ir); }
        SDL_Color lc = (i == choose_selection) ? cfg.theme.marked :
                       (act == ACT_CANCEL      ? cfg.theme.text_disabled : cfg.theme.text);
        const char *act_lbl = (choose_path_is_dir && act == ACT_INFO) ? "Dir Info" : act_labels[act];
        char lbl[64];
        if (i == choose_default)
            snprintf(lbl, sizeof(lbl), "%s  *", act_lbl);
        else
            snprintf(lbl, sizeof(lbl), "%s", act_lbl);
        draw_txt(font_menu, lbl, mx + isz + 20, iy + (spc - cfg.font_size_menu) / 2, lc);
    }
    SDL_RenderSetClipRect(renderer, NULL);
}

// ---------------------------------------------------------------------------
// File info modal
// ---------------------------------------------------------------------------
static void draw_fileinfo_modal() {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
    SDL_Rect scr = {0, 0, cfg.screen_w, cfg.screen_h};
    SDL_RenderFillRect(renderer, &scr);

    int lh  = cfg.font_size_menu + 8;
    int flh = cfg.font_size_footer + 8;
    int hh  = cfg.font_size_header + 16;
    int mw  = cfg.screen_w - 80;
    int mh  = hh + 10 + fileinfo_line_count * lh + 6 + flh + 8;
    int mx  = (cfg.screen_w - mw) / 2;
    int my  = (cfg.screen_h - mh) / 2;

    SDL_Rect mbg  = {mx, my, mw, mh};
    SDL_Rect body = {mx, my + hh, mw, mh - hh};
    SDL_SetRenderDrawColor(renderer, cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
    SDL_RenderFillRect(renderer, &mbg);
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_bg.r, cfg.theme.menu_bg.g, cfg.theme.menu_bg.b, cfg.theme.menu_bg.a);
    SDL_RenderFillRect(renderer, &body);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_border.r, cfg.theme.menu_border.g, cfg.theme.menu_border.b, 255);
    SDL_RenderDrawRect(renderer, &mbg);

    SDL_RenderSetClipRect(renderer, &mbg);

    int lx = mx + 18;
    if (tex_file) { SDL_Rect ir = {lx, my + (hh - 24) / 2, 24, 24}; SDL_RenderCopy(renderer, tex_file, NULL, &ir); lx += 30; }
    const char *fi_title = fileinfo_is_multi
        ? (fileinfo_is_dir ? "Dir Info" : "Selection Info")
        : (fileinfo_is_dir ? "Dir Info" : "File Info");
    draw_txt(font_header, fi_title, lx, my + (hh - cfg.font_size_header) / 2, cfg.theme.marked);

    int ty = my + hh + 10;
    for (int i = 0; i < fileinfo_line_count; i++) {
        draw_txt(font_menu, fileinfo_lines[i], mx + 18, ty, cfg.theme.text);
        ty += lh;
    }
    ty += 6;
    draw_txt(font_footer, "Press any button to close", mx + 18, ty, cfg.theme.text_disabled);

    SDL_RenderSetClipRect(renderer, NULL);
}


// ---------------------------------------------------------------------------
// About modal
// ---------------------------------------------------------------------------
static void draw_about_modal() {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
    SDL_Rect scr = {0, 0, cfg.screen_w, cfg.screen_h};
    SDL_RenderFillRect(renderer, &scr);

    int lh  = cfg.font_size_menu + 10;
    int flh = cfg.font_size_footer + 8;
    int hh  = cfg.font_size_header + 16;
    int mh  = hh + 14 + lh + lh + lh + lh + lh/2 + flh + 10;
    int mw  = cfg.screen_w - 80;
    int mx  = (cfg.screen_w - mw) / 2, my = (cfg.screen_h - mh) / 2;

    SDL_Rect mbg  = {mx, my, mw, mh};
    SDL_Rect body = {mx, my + hh, mw, mh - hh};
    SDL_SetRenderDrawColor(renderer, cfg.theme.header_bg.r, cfg.theme.header_bg.g,
                           cfg.theme.header_bg.b, 255);
    SDL_RenderFillRect(renderer, &mbg);
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_bg.r, cfg.theme.menu_bg.g,
                           cfg.theme.menu_bg.b, cfg.theme.menu_bg.a);
    SDL_RenderFillRect(renderer, &body);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_border.r, cfg.theme.menu_border.g,
                           cfg.theme.menu_border.b, 255);
    SDL_RenderDrawRect(renderer, &mbg);

    // Clip to modal so no text escapes the border
    SDL_RenderSetClipRect(renderer, &mbg);

    int lx = mx + 18;
    if (tex_about) { SDL_Rect ir = {lx, my + (hh - 24) / 2, 24, 24}; SDL_RenderCopy(renderer, tex_about, NULL, &ir); lx += 30; }
    char title_buf[32];
    snprintf(title_buf, sizeof(title_buf), "vTree Gold v" VTREE_VERSION);
    draw_txt(font_header, title_buf, lx, my + (hh - cfg.font_size_header) / 2, cfg.theme.marked);

    int ty = my + hh + 14;
    draw_txt(font_menu, "Gamepad-driven twin-panel file manager", mx + 18, ty, cfg.theme.text);
    ty += lh;
    draw_txt(font_menu, "Built with SDL2, SDL2_ttf, SDL2_image", mx + 18, ty, cfg.theme.text_disabled);
    ty += lh;
    draw_txt(font_menu, "Font: JetBrains Mono (OFL 1.1, JetBrains s.r.o.)", mx + 18, ty, cfg.theme.text_disabled);
    ty += lh;
    draw_txt(font_menu, "Icons: LineIcons (MIT, lineicons.com)", mx + 18, ty, cfg.theme.text_disabled);
    ty += lh + lh/2;
    draw_txt(font_footer, "Press any button to close", mx + 18, ty, cfg.theme.text_disabled);

    SDL_RenderSetClipRect(renderer, NULL); // restore
}

// ---------------------------------------------------------------------------
// Shell script detection
// ---------------------------------------------------------------------------
static bool is_shell_script(const char *name) {
    size_t n = strlen(name);
    return n >= 3 && strcmp(name + n - 3, ".sh") == 0;
}

// ---------------------------------------------------------------------------
// Symlink paste — creates symlinks from clipboard into destination pane
// ---------------------------------------------------------------------------
static void do_symlink(int pane_idx) {
    if (!fs_supports_symlinks(panes[pane_idx].current_path)) {
        snprintf(exec_error_title, sizeof(exec_error_title), "Cannot Create Symlink");
        snprintf(exec_error_msg, sizeof(exec_error_msg),
                 "Destination filesystem does not support symlinks");
        exec_error_active     = true;
        do_symlink_after_dest = false;
        paste_dest_active     = false;
        return;
    }
    AppState *s = &panes[pane_idx];
    for (int i = 0; i < clip.count; i++) {
        char dst[MAX_PATH];
        join_path(dst, s->current_path, clip.names[i]);
        if (access(dst, F_OK) == 0) {
            vtree_log("[symlink] skip existing: %s\n", dst);
            continue;
        }
        vtree_log("[symlink] %s -> %s\n", dst, clip.src_paths[i]);
        if (symlink(clip.src_paths[i], dst) != 0)
            vtree_log("[symlink] FAILED (errno %d: %s)\n", errno, strerror(errno));
    }
    clip.op = OP_NONE; clip.count = 0;
    do_symlink_after_dest = false;
    paste_dest_active     = false;
    load_dir(0, panes[0].current_path);
    load_dir(1, panes[1].current_path);
    current_mode = MODE_EXPLORER;
}

// ---------------------------------------------------------------------------
// Execute error modal
// ---------------------------------------------------------------------------
static void draw_exec_error_modal(void) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
    SDL_Rect scr = {0, 0, cfg.screen_w, cfg.screen_h};
    SDL_RenderFillRect(renderer, &scr);

    int hh  = cfg.font_size_header + 16;
    int lh  = cfg.font_size_menu + 10;
    int flh = cfg.font_size_footer + 8;
    int mw  = cfg.screen_w - 80;
    int mh  = hh + lh + flh + 16;
    int mx  = (cfg.screen_w - mw) / 2;
    int my  = (cfg.screen_h - mh) / 2;

    SDL_Rect mbg  = {mx, my, mw, mh};
    SDL_Rect body = {mx, my + hh, mw, mh - hh};
    SDL_SetRenderDrawColor(renderer, cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
    SDL_RenderFillRect(renderer, &mbg);
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_bg.r, cfg.theme.menu_bg.g, cfg.theme.menu_bg.b, cfg.theme.menu_bg.a);
    SDL_RenderFillRect(renderer, &body);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_border.r, cfg.theme.menu_border.g, cfg.theme.menu_border.b, 255);
    SDL_RenderDrawRect(renderer, &mbg);
    SDL_RenderSetClipRect(renderer, &mbg);

    draw_txt(font_header, exec_error_title, mx + 18, my + (hh - cfg.font_size_header) / 2, cfg.theme.marked);
    draw_txt(font_menu,   exec_error_msg,   mx + 18, my + hh + 8, cfg.theme.text);
    draw_txt(font_footer, "Press any button to close", mx + 18, my + hh + lh + 12, cfg.theme.text_disabled);
    SDL_RenderSetClipRect(renderer, NULL);
}

// ---------------------------------------------------------------------------
// Script executor — SDL teardown then execv
// ---------------------------------------------------------------------------
static void vtree_exec_script(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && !(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        snprintf(exec_error_title, sizeof(exec_error_title), "Cannot Execute");
        snprintf(exec_error_msg, sizeof(exec_error_msg),
                 "'%s' is not executable  (chmod +x to fix)", base);
        exec_error_active = true;
        current_mode = MODE_EXPLORER;
        return;
    }
    vtree_log("[exec] %s\n", path);

    /* MuOS mux_launch.sh scripts expect their containing directory as $1
       (the launcher always passes it so the script can cd into it and run
       the binary with a relative path).  Derive it from the script path. */
    char script_dir[MAX_PATH];
    strncpy(script_dir, path, MAX_PATH - 1);
    script_dir[MAX_PATH - 1] = '\0';
    char *last_slash = strrchr(script_dir, '/');
    if (last_slash && last_slash != script_dir) *last_slash = '\0';
    else if (!last_slash) strcpy(script_dir, ".");

    char *direct_args[] = { (char *)path, script_dir, NULL };
    char *sh_args[]     = { "sh", (char *)path, script_dir, NULL };

    /* Fork so the child can exec the script in a clean process while the
       parent tears down SDL fully and exits.  This ensures the display/
       audio/input devices are released before the new app tries to claim
       them — a plain execv() leaves SDL's fds alive in the replaced
       process long enough to race with the new app's SDL_Init. */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: close every fd we inherited above stderr so the script
           starts completely clean, then wait for the parent to finish
           releasing display/audio hardware before we exec. */
        for (int fd = 3; fd < 256; fd++) close(fd);
        struct timespec _ts = {0, 300000000L}; /* 300 ms */
        nanosleep(&_ts, NULL);
        execv(path, direct_args);
        execvp("sh", sh_args);
        _exit(127);
    }

    /* Parent: save dirs if requested (counts as a clean exit), then tear
       down SDL and wait for the child app to finish before exiting.
       muOS tracks the original process — staying alive keeps the session
       slot open so the frontend doesn't reload over the launched app. */
    if (cfg.remember_dirs) {
        strncpy(cfg.start_left,  panes[0].current_path, MAX_PATH - 1);
        strncpy(cfg.start_right, panes[1].current_path, MAX_PATH - 1);
        save_config();
    }
    if (debug_log_file) { fclose(debug_log_file); debug_log_file = NULL; }
    if (font_list)   TTF_CloseFont(font_list);
    if (font_header) TTF_CloseFont(font_header);
    if (font_footer) TTF_CloseFont(font_footer);
    if (font_menu)   TTF_CloseFont(font_menu);
    if (font_hex)    TTF_CloseFont(font_hex);
    if (renderer)    SDL_DestroyRenderer(renderer);
    if (window)      SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    if (pid > 0) { int st; waitpid(pid, &st, 0); }
    exit(0);
}

// ---------------------------------------------------------------------------
// Helper: open a file — auto-route images; show chooser for everything else
// ---------------------------------------------------------------------------
static void open_file(const char *path, const char *name) {
    strncpy(choose_path, path, MAX_PATH - 1);
    choose_count = 0;
    struct stat _st; bool is_dir = (stat(path, &_st) == 0 && S_ISDIR(_st.st_mode));
    choose_path_is_dir = is_dir;
    if (is_dir) {
        choose_default = 0;   // only Info offered; set default before pushing
    } else if (is_image_file(name)) {
        choose_actions[choose_count++] = ACT_IMG;
        choose_default  = 0;
    } else {
        bool text_known = is_text_file(name);
        choose_actions[choose_count++] = ACT_TEXT;
        choose_actions[choose_count++] = ACT_HEX;
        choose_default  = text_known ? 0 : 1;
        if (is_shell_script(name) && cfg.exec_scripts)
            choose_actions[choose_count++] = ACT_EXEC;
    }
    choose_actions[choose_count++] = ACT_INFO;
    choose_actions[choose_count++] = ACT_CANCEL;
    choose_selection = choose_default;
    fileinfo_active  = false;
    choose_pane      = active_pane;
    choose_marked    = 0;
    AppState *cs = &panes[active_pane];
    for (int i = 0; i < cs->file_count; i++)
        if (cs->files[i].marked) choose_marked++;
    current_mode     = MODE_VIEW_CHOOSE;
}

// ---------------------------------------------------------------------------
// Paste conflict resolution
// ---------------------------------------------------------------------------
typedef enum { PC_OVERWRITE = 0, PC_KEEP_BOTH = 1, PC_SKIP = 2, PC_CANCEL = 3 } PasteConflict;
#define PC_MAX 4
static const char *pc_labels[PC_MAX] = { "Overwrite", "Keep Both", "Skip", "Cancel" };

// Generate a unique destination path: "stem (copy).ext", "stem (copy 2).ext", ...
static void make_copy_path(const char *dir, const char *name, char *out) {
    char stem[256], ext[64];
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        int slen = (int)(dot - name);
        if (slen > 255) slen = 255;
        strncpy(stem, name, slen); stem[slen] = '\0';
        strncpy(ext, dot, 63); ext[63] = '\0';
    } else {
        strncpy(stem, name, 255); stem[255] = '\0';
        ext[0] = '\0';
    }
    char candidate[MAX_PATH];
    snprintf(candidate, MAX_PATH, "%s/%s (copy)%s", dir, stem, ext);
    if (access(candidate, F_OK) != 0) { strcpy(out, candidate); return; }
    for (int n = 2; n < 100; n++) {
        snprintf(candidate, MAX_PATH, "%s/%s (copy %d)%s", dir, stem, n, ext);
        if (access(candidate, F_OK) != 0) { strcpy(out, candidate); return; }
    }
    strcpy(out, candidate); // fallback (copy 100) — shouldn't happen
}

// True if panes[pane_idx].files[file_idx] is currently in the clipboard
static bool file_in_clipboard(int pane_idx, int file_idx) {
    AppState *s = &panes[pane_idx];
    char full[MAX_PATH];
    join_path(full, s->current_path, s->files[file_idx].name);
    for (int c = 0; c < clip.count; c++)
        if (strcmp(clip.src_paths[c], full) == 0) return true;
    return false;
}

// Count clipboard items whose destination already exists in the given pane
static int count_paste_conflicts(int pane_idx) {
    AppState *s = &panes[pane_idx];
    int n = 0;
    for (int i = 0; i < clip.count; i++) {
        char d[MAX_PATH]; join_path(d, s->current_path, clip.names[i]);
        if (access(d, F_OK) == 0) n++;
    }
    return n;
}

// Execute paste using the chosen conflict resolution into the given pane
static void do_paste(PasteConflict res, int pane_idx) {
    if (res == PC_CANCEL) { paste_conflict_active = false; paste_dest_active = false; return; }
    AppState *s = &panes[pane_idx];
    for (int i = 0; i < clip.count; i++) {
        char d[MAX_PATH]; join_path(d, s->current_path, clip.names[i]);

        // Self-copy is a no-op unless the user chose Keep Both (which renames
        // the destination to a "(copy)" path, making it safe to proceed).
        if (clip.op == OP_COPY && strcmp(clip.src_paths[i], d) == 0 && res != PC_KEEP_BOTH) continue;

        bool conflict = (access(d, F_OK) == 0);
        if (conflict) {
            if (res == PC_SKIP) continue;
            if (res == PC_KEEP_BOTH) make_copy_path(s->current_path, clip.names[i], d);
            // PC_OVERWRITE: d unchanged — existing file will be replaced
        }
        if (clip.op == OP_COPY) {
            copy_path(clip.src_paths[i], d);
        } else {
            if (rename(clip.src_paths[i], d) != 0) {
                copy_path(clip.src_paths[i], d);
                delete_path(clip.src_paths[i]);
            }
        }
    }
    clip.op = OP_NONE; clip.count = 0;
    paste_conflict_active = false;
    paste_dest_active     = false;
    load_dir(0, panes[0].current_path); load_dir(1, panes[1].current_path);
    current_mode = MODE_EXPLORER;
}

// ---------------------------------------------------------------------------
// Paste modal draw helpers (called from render loop)
// ---------------------------------------------------------------------------
static void draw_paste_dest(void) {
    int hh  = cfg.font_size_footer + 14;
    int spc = cfg.font_size_menu   + 14;
    int mw  = cfg.screen_w - 80;
    int mh  = hh + 2 * spc + 8;
    int mx  = (cfg.screen_w - mw) / 2;
    int my  = (cfg.screen_h - mh) / 2;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
    SDL_Rect scr = {0, 0, cfg.screen_w, cfg.screen_h};
    SDL_RenderFillRect(renderer, &scr);
    SDL_Rect mbg = {mx, my, mw, mh};
    SDL_Rect bdy = {mx, my + hh, mw, mh - hh};
    SDL_SetRenderDrawColor(renderer, cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
    SDL_RenderFillRect(renderer, &mbg);
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_bg.r, cfg.theme.menu_bg.g, cfg.theme.menu_bg.b, cfg.theme.menu_bg.a);
    SDL_RenderFillRect(renderer, &bdy);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_border.r, cfg.theme.menu_border.g, cfg.theme.menu_border.b, 255);
    SDL_RenderDrawRect(renderer, &mbg);
    SDL_RenderSetClipRect(renderer, &mbg);

    draw_txt(font_footer, do_symlink_after_dest ? "Symlink Destination:" : "Paste Destination:",
             mx + 12, my + (hh - cfg.font_size_footer) / 2, cfg.theme.text_disabled);

    const char *dest_labels[2] = { "This pane", "Other pane" };
    for (int i = 0; i < 2; i++) {
        int pane_i = (i == 0) ? active_pane : (1 - active_pane);
        int iy = my + hh + i * spc;
        if (i == paste_dest_sel) {
            SDL_SetRenderDrawColor(renderer, cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g, cfg.theme.highlight_bg.b, 255);
            SDL_Rect row = {mx + 2, iy - 2, mw - 4, spc};
            SDL_RenderFillRect(renderer, &row);
        }
        SDL_Color lc = (i == paste_dest_sel) ? cfg.theme.marked : cfg.theme.text;
        char line[MAX_PATH + 16];
        snprintf(line, sizeof(line), "%-10s  %s", dest_labels[i], panes[pane_i].current_path);
        draw_txt(font_menu, line, mx + 16, iy + (spc - cfg.font_size_menu) / 2, lc);
    }
    SDL_RenderSetClipRect(renderer, NULL);
}

static void draw_paste_conflict(void) {
    int hh  = cfg.font_size_footer + 14;
    int spc = cfg.font_size_menu   + 14;
    int mw  = cfg.screen_w - 80;
    int mh  = hh + PC_MAX * spc + 8;
    int mx  = (cfg.screen_w - mw) / 2;
    int my  = (cfg.screen_h - mh) / 2;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
    SDL_Rect scr = {0, 0, cfg.screen_w, cfg.screen_h};
    SDL_RenderFillRect(renderer, &scr);
    SDL_Rect mbg = {mx, my, mw, mh};
    SDL_Rect bdy = {mx, my + hh, mw, mh - hh};
    SDL_SetRenderDrawColor(renderer, cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
    SDL_RenderFillRect(renderer, &mbg);
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_bg.r, cfg.theme.menu_bg.g, cfg.theme.menu_bg.b, cfg.theme.menu_bg.a);
    SDL_RenderFillRect(renderer, &bdy);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, cfg.theme.menu_border.r, cfg.theme.menu_border.g, cfg.theme.menu_border.b, 255);
    SDL_RenderDrawRect(renderer, &mbg);
    SDL_RenderSetClipRect(renderer, &mbg);

    char title[48];
    snprintf(title, sizeof(title), "%d File(s) Already Exist", paste_conflict_count);
    draw_txt(font_footer, title, mx + 12,
             my + (hh - cfg.font_size_footer) / 2, cfg.theme.text_disabled);

    for (int i = 0; i < PC_MAX; i++) {
        int iy = my + hh + i * spc;
        if (i == paste_conflict_sel) {
            SDL_SetRenderDrawColor(renderer, cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g, cfg.theme.highlight_bg.b, 255);
            SDL_Rect row = {mx + 2, iy - 2, mw - 4, spc};
            SDL_RenderFillRect(renderer, &row);
        }
        SDL_Color lc = (i == paste_conflict_sel) ? cfg.theme.marked : cfg.theme.text;
        if (i == PC_CANCEL && i != paste_conflict_sel) lc = cfg.theme.text_disabled;
        draw_txt(font_menu, pc_labels[i], mx + 16, iy + (spc - cfg.font_size_menu) / 2, lc);
    }
    SDL_RenderSetClipRect(renderer, NULL);
}

// ---------------------------------------------------------------------------
// Rotation helper — blits the render_target to the physical screen rotated.
// When render_target is NULL (no rotation) this is a plain SDL_RenderPresent.
// ---------------------------------------------------------------------------
static void present_frame(void) {
    if (!render_target) {
        SDL_RenderPresent(renderer);
        return;
    }
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    double angle = cfg.rotation * 90.0;
    if (cfg.rotation == 2) {
        // 180°: logical == physical size, so a simple scaled copy works
        SDL_Rect dst = {0, 0, phys_w, phys_h};
        SDL_RenderCopyEx(renderer, render_target, NULL, &dst, angle, NULL, SDL_FLIP_NONE);
    } else {
        // 90° or 270°: logical is (phys_h × phys_w).
        // Place dst so its centre aligns with the physical screen centre;
        // after rotation the texture fills the physical screen exactly.
        SDL_Rect dst = {phys_w/2 - phys_h/2, phys_h/2 - phys_w/2, phys_h, phys_w};
        SDL_RenderCopyEx(renderer, render_target, NULL, &dst, angle, NULL, SDL_FLIP_NONE);
    }
    SDL_RenderPresent(renderer);
    // Restore render target for the next frame
    SDL_SetRenderTarget(renderer, render_target);
}

// ---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    // ── Argument parsing ──────────────────────────────────────────────────────
    int cmdline_rotation = -1;  // -1 = not set; overrides Rotation= in config.ini when >= 0
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("vTree " VTREE_VERSION " — dual-pane file manager for gamepad devices\n\n");
            printf("Usage: vtree [options]\n\n");
            printf("Options:\n");
            printf("  -h, --help               Show this help and exit\n");
            printf("  -v, --version            Show version and exit\n");
            printf("  --debug                  Enable debug logging to stdout\n");
            printf("  --logfile <file>         Write debug log to <file> (implies --debug)\n");
            printf("  --logfile=<file>         Same as above, alternative syntax\n");
            printf("  --rotate=<0-3>           Override display rotation: 0=none, 1=90 CW, 2=180, 3=270 CW\n");
            printf("\nFiles (loaded from the directory containing the vtree binary):\n");
            printf("  config.ini               Main configuration (keys, display, active theme)\n");
            printf("  theme.ini                Additional named theme presets\n");
            printf("  fonts/                   Font files (.ttf/.otf) — first found is used if\n");
            printf("                           FontFile= is blank in config.ini\n");
            printf("\nDebug log includes: startup info, SDL/TTF/image versions, directory loads,\n");
            printf("file operations (copy, move, delete, rename), and any errors encountered.\n");
            return 0;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("vTree " VTREE_VERSION "\n");
            return 0;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug_mode = true;
        } else if (strcmp(argv[i], "--logfile") == 0 && i + 1 < argc) {
            debug_log_file = fopen(argv[++i], "w");
            debug_mode = true;  // --logfile implies --debug
        } else if (strncmp(argv[i], "--logfile=", 10) == 0) {
            debug_log_file = fopen(argv[i] + 10, "w");
            debug_mode = true;
        } else if (strncmp(argv[i], "--rotate=", 9) == 0) {
            int r = atoi(argv[i] + 9);
            if (r >= 0 && r <= 3) cmdline_rotation = r;
            else { fprintf(stderr, "vtree: --rotate value must be 0-3\n"); return 1; }
        } else {
            fprintf(stderr, "vtree: unknown option '%s'\n", argv[i]);
            fprintf(stderr, "Try 'vtree --help' for usage.\n");
            return 1;
        }
    }
    if (debug_mode) setvbuf(stdout, NULL, _IONBF, 0);
    vtree_log("vTree Gold " VTREE_VERSION " starting\n");
    if (debug_log_file) vtree_log("Log file opened\n");

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO);
    TTF_Init(); IMG_Init(IMG_INIT_PNG);

    // Log SDL/TTF/IMG versions
    if (debug_mode) {
        SDL_version sv; SDL_GetVersion(&sv);
        const SDL_version *tv = TTF_Linked_Version();
        const SDL_version *iv = IMG_Linked_Version();
        vtree_log("SDL %d.%d.%d  SDL_ttf %d.%d.%d  SDL_image %d.%d.%d\n",
                  sv.major, sv.minor, sv.patch,
                  tv->major, tv->minor, tv->patch,
                  iv->major, iv->minor, iv->patch);
    }

    load_config();
    if (cmdline_rotation >= 0) cfg.rotation = cmdline_rotation;  // CLI overrides config.ini
    scan_fonts();   // builds font_files[] and sets current_font_idx from cfg.font_path

    // Log config / display / theme
    if (debug_mode) {
        vtree_log("Screen: %dx%d\n", cfg.screen_w, cfg.screen_h);
        vtree_log("Start dirs: left=%s  right=%s\n", cfg.start_left, cfg.start_right);
        if (current_named_theme >= 0 && current_named_theme < named_theme_count)
            vtree_log("Active theme: %s\n", named_themes[current_named_theme].name);
        else
            vtree_log("Active theme: (none / fallback)\n");
        vtree_log("Themes loaded: %d\n", named_theme_count);

        vtree_log("Fonts found: %d\n", font_file_count);
        for (int i = 0; i < font_file_count; i++)
            vtree_log("  [%d] %s%s\n", i, font_files[i], i == current_font_idx ? "  <active>" : "");
    }

    // Load gamecontrollerdb — prefer path from config, fall back to CWD default
    {
        const char *db = cfg.gamecontrollerdb[0] ? cfg.gamecontrollerdb : "gamecontrollerdb.txt";
        if (SDL_GameControllerAddMappingsFromFile(db) >= 0)
            vtree_log("Gamecontrollerdb loaded: %s\n", db);
        else
            vtree_log("Gamecontrollerdb not found: %s\n", db);
    }

    // At this point cfg.screen_w/h are physical (auto-detected or explicitly set as physical).
    // Create the window at physical size, then compute logical dims from rotation.
    window   = SDL_CreateWindow("vTree Gold", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, cfg.screen_w, cfg.screen_h, 0);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    // Store physical dims; swap cfg.screen_w/h to logical if rotating 90° or 270°
    SDL_GetRendererOutputSize(renderer, &phys_w, &phys_h);
    if (phys_w <= 0) phys_w = cfg.screen_w;
    if (phys_h <= 0) phys_h = cfg.screen_h;
    if (cfg.rotation == 1 || cfg.rotation == 3) {
        cfg.screen_w = phys_h;
        cfg.screen_h = phys_w;
    }
    // Create off-screen render target for rotation (logical size)
    if (cfg.rotation != 0) {
        render_target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_TARGET,
                                          cfg.screen_w, cfg.screen_h);
        if (render_target) SDL_SetRenderTarget(renderer, render_target);
    }
    vtree_log("Window: %dx%d (physical)  logical: %dx%d  rotation: %d\n",
              phys_w, phys_h, cfg.screen_w, cfg.screen_h, cfg.rotation);

    {
        if (font_file_count == 0) {
            vtree_log("WARNING: no fonts found in ./fonts/ — text will not render\n");
        } else {
        const char *fp = font_files[current_font_idx];
        font_list   = TTF_OpenFont(fp, cfg.font_size_list);
        font_header = TTF_OpenFont(fp, cfg.font_size_header);
        font_footer = TTF_OpenFont(fp, cfg.font_size_footer);
        font_menu   = TTF_OpenFont(fp, cfg.font_size_menu);
        font_hex    = TTF_OpenFont(fp, cfg.font_size_hex);
        vtree_log("Font: %s\n", fp);
        vtree_log("  list=%dpt %s  header=%dpt %s  footer=%dpt %s  menu=%dpt %s  hex=%dpt %s\n",
                  cfg.font_size_list,   font_list   ? "OK" : "FAILED",
                  cfg.font_size_header, font_header ? "OK" : "FAILED",
                  cfg.font_size_footer, font_footer ? "OK" : "FAILED",
                  cfg.font_size_menu,   font_menu   ? "OK" : "FAILED",
                  cfg.font_size_hex,    font_hex    ? "OK" : "FAILED");
        } // end font_file_count > 0
    }

    // Load textures and warn on any that fail
    struct { SDL_Texture **t; const char *path; } tex_list[] = {
        {&tex_file,      "res/file.png"},    {&tex_folder,   "res/folder.png"},
        {&tex_img,       "res/img.png"},     {&tex_txt,      "res/txt.png"},
        {&tex_dirup,     "res/dirup.png"},   {&tex_copy,     "res/copy.png"},
        {&tex_cut,       "res/cut.png"},     {&tex_paste,    "res/paste.png"},
        {&tex_symlink,   "res/symlink.png"},
        {&tex_rename,    "res/rename.png"},  {&tex_delete,   "res/delete.png"},
        {&tex_settings,  "res/settings.png"},{&tex_exit,     "res/exit.png"},
        {&tex_newfile,   "res/newfile.png"}, {&tex_newfolder,"res/newfolder.png"},
        {&tex_about,     "res/about.png"},   {&tex_enterfol, "res/enterfol.png"},
        {&tex_viewer,    "res/viewer.png"},  {&tex_hexview,  "res/hexview.png"},
        {&tex_imgview,   "res/imgview.png"}, {&tex_fileinfo, "res/fileinfo.png"},
        {&tex_exec,      "res/exec.png"},
    };
    int tex_ok = 0, tex_missing = 0;
    for (int i = 0; i < (int)(sizeof(tex_list)/sizeof(tex_list[0])); i++) {
        *tex_list[i].t = IMG_LoadTexture(renderer, tex_list[i].path);
        if (*tex_list[i].t) tex_ok++;
        else { tex_missing++; vtree_log("  MISSING texture: %s\n", tex_list[i].path); }
    }
    vtree_log("Textures: %d/%d loaded%s\n", tex_ok,
              (int)(sizeof(tex_list)/sizeof(tex_list[0])),
              tex_missing ? " (see MISSING lines above)" : "");

    pad = NULL;
    int nj = SDL_NumJoysticks();
    vtree_log("Joysticks found: %d\n", nj);
    for (int i = 0; i < nj; ++i) {
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
            if (pad) vtree_log("Gamepad opened: %s\n", SDL_GameControllerName(pad));
            else     vtree_log("Gamepad open FAILED (joystick %d)\n", i);
            break;
        }
    }
    if (!pad) vtree_log("No gamepad found — keyboard/touch only\n");

    load_dir(0, cfg.start_left);
    load_dir(1, cfg.start_right);

    int  menu_selection = 0;
    int  filemenu_sel   = 0;
    bool menu_in_files  = false;
    bool about_active       = false;
    bool about_r1_held      = false;  // combo state for about-screen easter egg
    bool about_dpad_up_held = false;
    bool about_combo_fired  = false;
    bool running = true;
    SDL_Event ev;

    bool   dpad_up_held    = false, dpad_down_held   = false;
    bool   dpad_left_held  = false, dpad_right_held  = false;
    bool   osk_bksp_held   = false;   // backspace auto-repeat in OSK
    // Dedicated OSK caret held flags — separate from dpad so they never alias
    bool   osk_cur_l_held  = false, osk_cur_r_held  = false;
    Uint32 next_up_tick    = 0, next_down_tick   = 0;
    Uint32 next_left_tick  = 0, next_right_tick  = 0;
    Uint32 next_bksp_tick  = 0;
    Uint32 next_cur_l_tick = 0, next_cur_r_tick  = 0;
    const Uint32 REPEAT_DELAY = 400, REPEAT_RATE = 60;

    while (running) {
        Uint32 now = SDL_GetTicks();

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                if (cfg.remember_dirs) {
                    strncpy(cfg.start_left,  panes[0].current_path, MAX_PATH - 1);
                    strncpy(cfg.start_right, panes[1].current_path, MAX_PATH - 1);
                    save_config();
                }
                running = false;
            }

            if (ev.type == SDL_CONTROLLERBUTTONUP) {
                SDL_GameControllerButton b = ev.cbutton.button;
                if (b == SDL_CONTROLLER_BUTTON_DPAD_UP)       dpad_up_held    = false;
                if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN)     dpad_down_held  = false;
                if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT)     dpad_left_held  = false;
                if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)    dpad_right_held = false;
                if (b == cfg.osk_k_bksp)                      osk_bksp_held   = false;
                if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)  osk_cur_l_held  = false;
                if (b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) osk_cur_r_held  = false;
                // R1 release on about screen: dismiss if combo wasn't triggered
                if (b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER && about_active) {
                    if (!about_combo_fired) about_active = false;
                    about_r1_held = about_dpad_up_held = about_combo_fired = false;
                }
            }

            if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                SDL_GameControllerButton btn = ev.cbutton.button;
                AppState *s = &panes[active_pane];

                // ── SNAKE ────────────────────────────────────────────────────
                if (current_mode == MODE_SNAKE) {
                    snake_handle_button(btn);
                    continue;
                }

                // ── EXPLORER ────────────────────────────────────────────────
                if (current_mode == MODE_EXPLORER) {
                    if (exec_error_active) { exec_error_active = false; continue; }
                    if (fileinfo_active)   { fileinfo_active   = false; continue; }
                    if (btn == cfg.k_menu) {
                        current_mode = MODE_CONTEXT_MENU; menu_selection = 0;
                        filemenu_sel = 0; menu_in_files = false; about_active = false;
                        delete_confirm_active = false;
                    } else if (btn == cfg.k_mark) {
                        if (strcmp(s->files[s->selected_index].name, "..") != 0) {
                            s->files[s->selected_index].marked = !s->files[s->selected_index].marked;
                            // advance one row after marking to allow rapid multi-select
                            if (s->selected_index < s->file_count - 1) s->selected_index++;
                        }
                    } else if (btn == cfg.k_pgup) {
                        int item_h = (font_list ? TTF_FontHeight(font_list) : cfg.font_size_list) + 6;
                        int page   = (cfg.screen_h - cfg.font_size_header - cfg.font_size_footer - 28) / item_h;
                        s->selected_index = SDL_max(0, s->selected_index - SDL_max(1, page));
                    } else if (btn == cfg.k_pgdn) {
                        int item_h = (font_list ? TTF_FontHeight(font_list) : cfg.font_size_list) + 6;
                        int page   = (cfg.screen_h - cfg.font_size_header - cfg.font_size_footer - 28) / item_h;
                        s->selected_index = SDL_min(s->file_count - 1, s->selected_index + SDL_max(1, page));
                    } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                        if (s->selected_index > 0) s->selected_index--;
                        else if (s->file_count > 0) s->selected_index = s->file_count - 1;
                        dpad_up_held = true; next_up_tick = now + REPEAT_DELAY;
                    } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                        if (s->selected_index < s->file_count - 1) s->selected_index++;
                        else s->selected_index = 0;
                        dpad_down_held = true; next_down_tick = now + REPEAT_DELAY;
                    } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT && !cfg.single_pane) {
                        active_pane = 0;
                    } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && !cfg.single_pane) {
                        active_pane = 1;
                    } else if (btn == cfg.k_confirm) {
                        FileEntry *fe = &s->files[s->selected_index];
                        int num_marked = 0;
                        for (int i = 0; i < s->file_count; i++)
                            if (s->files[i].marked) num_marked++;
                        if (fe->is_dir && strcmp(fe->name, "..") != 0 && num_marked > 0) {
                            // multi-select active: show info chooser instead of entering dir
                            char full[MAX_PATH]; join_path(full, s->current_path, fe->name);
                            open_file(full, fe->name);
                        } else if (fe->is_dir) {
                            char next[MAX_PATH];
                            if (strcmp(fe->name, "..") == 0) {
                                char *last = strrchr(s->current_path, '/');
                                if (last == s->current_path) strcpy(next, "/");
                                else { *last = '\0'; strcpy(next, s->current_path); }
                            } else { join_path(next, s->current_path, fe->name); }
                            load_dir(active_pane, next);
                        } else {
                            char full[MAX_PATH];
                            join_path(full, s->current_path, fe->name);
                            open_file(full, fe->name);
                        }
                    } else if (btn == cfg.k_back) {
                        // if any files are marked, first B press clears marks only
                        bool any_marked = false;
                        for (int i = 0; i < s->file_count; i++)
                            if (s->files[i].marked) { any_marked = true; break; }
                        if (any_marked) {
                            for (int i = 0; i < s->file_count; i++) s->files[i].marked = false;
                        } else {
                            char next[MAX_PATH];
                            char *last = strrchr(s->current_path, '/');
                            if (last == s->current_path) strcpy(next, "/");
                            else { *last = '\0'; strcpy(next, s->current_path); }
                            load_dir(active_pane, next);
                        }
                    }

                // ── CONTEXT MENU ─────────────────────────────────────────────
                } else if (current_mode == MODE_CONTEXT_MENU) {
                    // ── Destination picker eats input first ───────────────────
                    if (paste_dest_active) {
                        if (btn == cfg.k_back || btn == cfg.k_menu) {
                            paste_dest_active     = false;
                            do_symlink_after_dest = false;
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP || btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
                            paste_dest_sel = 0;
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN || btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                            paste_dest_sel = 1;
                        } else if (btn == cfg.k_confirm) {
                            paste_dest_pane   = (paste_dest_sel == 0) ? active_pane : (1 - active_pane);
                            paste_dest_active = false;
                            if (do_symlink_after_dest) {
                                do_symlink(paste_dest_pane);
                            } else {
                                paste_conflict_count = count_paste_conflicts(paste_dest_pane);
                                if (paste_conflict_count > 0) {
                                    paste_conflict_active = true;
                                    paste_conflict_sel    = 0;
                                } else {
                                    do_paste(PC_OVERWRITE, paste_dest_pane);
                                }
                            }
                        }
                    // ── Conflict modal ────────────────────────────────────────
                    } else if (paste_conflict_active) {
                        if (btn == cfg.k_back || btn == cfg.k_menu) {
                            paste_conflict_active = false;
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                            paste_conflict_sel = (paste_conflict_sel - 1 + PC_MAX) % PC_MAX;
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                            paste_conflict_sel = (paste_conflict_sel + 1) % PC_MAX;
                        } else if (btn == cfg.k_confirm) {
                            do_paste((PasteConflict)paste_conflict_sel, paste_dest_pane);
                        }
                    // ── About modal ───────────────────────────────────────────
                    } else if (about_active) {
                        if (btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                            about_r1_held = true;   // silent — wait for combo or release
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP && about_r1_held) {
                            about_dpad_up_held = true;  // still silent
                        } else if (btn == SDL_CONTROLLER_BUTTON_A && about_r1_held && about_dpad_up_held) {
                            // Combo: R1 + DpadUp + A → snake
                            about_combo_fired = true;
                            about_active = false;
                            about_r1_held = about_dpad_up_held = false;
                            snake_enter();
                            current_mode = MODE_SNAKE;
                        } else {
                            about_active = false;  // any other button dismisses
                            about_r1_held = about_dpad_up_held = about_combo_fired = false;
                        }
                    // ── File ops submenu ──────────────────────────────────────
                    } else if (menu_in_files) {
                        // items that cannot operate on ".." when nothing else is marked
                        bool any_m = false;
                        for (int i = 0; i < s->file_count; i++) if (s->files[i].marked) { any_m = true; break; }
                        bool dotdot_sel = (!any_m && s->file_count > 0 &&
                                          strcmp(s->files[s->selected_index].name, "..") == 0);
#define DOTDOT_SKIP(sel) (dotdot_sel && ((sel)==FILEMENU_COPY||(sel)==FILEMENU_CUT|| \
                                         (sel)==FILEMENU_RENAME||(sel)==FILEMENU_DELETE))
                        if (btn == cfg.k_menu || btn == cfg.k_back) {
                            menu_in_files = false; delete_confirm_active = false;
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                            filemenu_sel = (filemenu_sel - 1 + FILEMENU_MAX) % FILEMENU_MAX;
                            while ((filemenu_sel == FILEMENU_PASTE   && clip.op == OP_NONE) ||
                                   (filemenu_sel == FILEMENU_SYMLINK && (clip.op != OP_COPY || clip.count == 0 ||
                                       (!fs_supports_symlinks(panes[0].current_path) &&
                                        !fs_supports_symlinks(panes[1].current_path)))) ||
                                   DOTDOT_SKIP(filemenu_sel))
                                filemenu_sel = (filemenu_sel - 1 + FILEMENU_MAX) % FILEMENU_MAX;
                            delete_confirm_active = false;
                            dpad_up_held = true; next_up_tick = now + REPEAT_DELAY;
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                            filemenu_sel = (filemenu_sel + 1) % FILEMENU_MAX;
                            while ((filemenu_sel == FILEMENU_PASTE   && clip.op == OP_NONE) ||
                                   (filemenu_sel == FILEMENU_SYMLINK && (clip.op != OP_COPY || clip.count == 0 ||
                                       (!fs_supports_symlinks(panes[0].current_path) &&
                                        !fs_supports_symlinks(panes[1].current_path)))) ||
                                   DOTDOT_SKIP(filemenu_sel))
                                filemenu_sel = (filemenu_sel + 1) % FILEMENU_MAX;
                            delete_confirm_active = false;
                            dpad_down_held = true; next_down_tick = now + REPEAT_DELAY;
                        } else if (btn == cfg.k_confirm) {
                            if (filemenu_sel == FILEMENU_BACK) {
                                menu_in_files = false; delete_confirm_active = false;
                            } else if (filemenu_sel == FILEMENU_COPY || filemenu_sel == FILEMENU_CUT) {
                                clip.count = 0;
                                clip.op = (filemenu_sel == FILEMENU_COPY) ? OP_COPY : OP_CUT;
                                for (int i = 0; i < s->file_count; i++) {
                                    if (s->files[i].marked && clip.count < MAX_CLIPBOARD) {
                                        join_path(clip.src_paths[clip.count], s->current_path, s->files[i].name);
                                        strcpy(clip.names[clip.count], s->files[i].name);
                                        clip.count++; s->files[i].marked = false;
                                    }
                                }
                                if (clip.count == 0 && strcmp(s->files[s->selected_index].name, "..") != 0) {
                                    join_path(clip.src_paths[0], s->current_path, s->files[s->selected_index].name);
                                    strcpy(clip.names[0], s->files[s->selected_index].name); clip.count = 1;
                                }
                                current_mode = MODE_EXPLORER;
                            } else if (filemenu_sel == FILEMENU_PASTE && clip.op != OP_NONE) {
                                bool show_dest_dialog = !cfg.single_pane &&
                                    strcmp(panes[0].current_path, panes[1].current_path) != 0;
                                if (show_dest_dialog) {
                                    paste_dest_active = true;
                                    paste_dest_sel    = 0;
                                } else {
                                    paste_dest_pane      = active_pane;
                                    paste_conflict_count = count_paste_conflicts(paste_dest_pane);
                                    if (paste_conflict_count > 0) {
                                        paste_conflict_active = true;
                                        paste_conflict_sel    = 0;
                                    } else {
                                        do_paste(PC_OVERWRITE, paste_dest_pane);
                                    }
                                }
                            } else if (filemenu_sel == FILEMENU_SYMLINK && clip.op == OP_COPY && clip.count > 0) {
                                bool p0_ok = fs_supports_symlinks(panes[0].current_path);
                                bool p1_ok = fs_supports_symlinks(panes[1].current_path);
                                if (!p0_ok && !p1_ok) {
                                    // Neither pane supports symlinks — error immediately
                                    current_mode = MODE_EXPLORER;
                                    snprintf(exec_error_title, sizeof(exec_error_title), "Cannot Create Symlink");
                                    snprintf(exec_error_msg, sizeof(exec_error_msg),
                                             "Destination filesystem does not support symlinks");
                                    exec_error_active = true;
                                } else {
                                    do_symlink_after_dest = true;
                                    bool diff_paths = !cfg.single_pane &&
                                        strcmp(panes[0].current_path, panes[1].current_path) != 0;
                                    if (diff_paths && p0_ok && p1_ok) {
                                        // Both valid — let user choose destination
                                        paste_dest_active = true;
                                        paste_dest_sel    = 0;
                                    } else if (diff_paths) {
                                        // Only one pane is a valid destination — route there directly
                                        do_symlink(p0_ok ? 0 : 1);
                                    } else {
                                        do_symlink(active_pane);
                                    }
                                }
                            } else if (filemenu_sel == FILEMENU_RENAME) {
                                FileEntry *fe = &s->files[s->selected_index];
                                if (strcmp(fe->name, "..") != 0) osk_enter(s->current_path, fe->name);
                            } else if (filemenu_sel == FILEMENU_DELETE) {
                                if (!delete_confirm_active) {
                                    delete_confirm_active = true;
                                } else {
                                    int deleted = 0;
                                    for (int i = 0; i < s->file_count; i++) {
                                        if (s->files[i].marked) { char t[MAX_PATH]; join_path(t, s->current_path, s->files[i].name); delete_path(t); deleted++; }
                                    }
                                    if (deleted == 0 && strcmp(s->files[s->selected_index].name, "..") != 0) { char t[MAX_PATH]; join_path(t, s->current_path, s->files[s->selected_index].name); delete_path(t); }
                                    load_dir(0, panes[0].current_path); load_dir(1, panes[1].current_path);
                                    delete_confirm_active = false; current_mode = MODE_EXPLORER;
                                }
                            } else if (filemenu_sel == FILEMENU_NEWFILE) {
                                osk_enter_new(s->current_path, false);
                            } else if (filemenu_sel == FILEMENU_NEWDIR) {
                                osk_enter_new(s->current_path, true);
                            }
                        }
                    // ── Top-level menu ────────────────────────────────────────
                    } else {
                        if (btn == cfg.k_menu || btn == cfg.k_back) {
                            current_mode = MODE_EXPLORER; delete_confirm_active = false; paste_conflict_active = false;
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                            menu_selection = (menu_selection - 1 + TOPMENU_MAX) % TOPMENU_MAX;
                            dpad_up_held = true; next_up_tick = now + REPEAT_DELAY;
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                            menu_selection = (menu_selection + 1) % TOPMENU_MAX;
                            dpad_down_held = true; next_down_tick = now + REPEAT_DELAY;
                        } else if (btn == cfg.k_confirm) {
                            if (menu_selection == TOPMENU_FILES) {
                                menu_in_files = true; filemenu_sel = 0;
                            } else if (menu_selection == TOPMENU_SETTINGS) {
                                cfg_snapshot       = cfg;
                                snapshot_theme_idx = current_named_theme;
                                snapshot_font_idx  = current_font_idx;
                                pending_keys_from_cfg();
                                settings_tab = 0;
                                settings_tab_indices[0] = settings_tab_indices[1] = settings_tab_indices[2] = 0;
                                settings_index = 0; current_mode = MODE_SETTINGS;
                            } else if (menu_selection == TOPMENU_ABOUT) {
                                about_active = true;
                            } else if (menu_selection == TOPMENU_EXIT) {
                                if (cfg.remember_dirs) {
                                    strncpy(cfg.start_left,  panes[0].current_path, MAX_PATH - 1);
                                    strncpy(cfg.start_right, panes[1].current_path, MAX_PATH - 1);
                                    save_config();
                                }
                                running = false;
                            }
                        }
                    }

                // ── SETTINGS ─────────────────────────────────────────────────
                } else if (current_mode == MODE_SETTINGS) {
                    if (settings_save_prompt) {
                        if (btn == cfg.k_back || btn == cfg.k_menu) {
                            settings_save_prompt = false;  // cancel prompt, stay in settings
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP || btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
                            save_prompt_sel = 0;
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN || btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                            save_prompt_sel = 1;
                        } else if (btn == cfg.k_confirm) {
                            if (save_prompt_sel == 0) {
                                pending_keys_to_cfg();
                                save_config();
                                cfg_snapshot       = cfg;
                                snapshot_theme_idx = current_named_theme;
                                snapshot_font_idx  = current_font_idx;
                                settings_dirty = false;
                                settings_save_toast = SDL_GetTicks() + 1800;
                            }
                            settings_do_close();
                        }
                    } else if (settings_listening) {
                        // Check for duplicate within the same binding group only
                        bool conflict = false;
                        int grp_n; SDL_GameControllerButton **grp = pending_group_for(settings_listen_target, &grp_n);
                        for (int _i = 0; grp && _i < grp_n; _i++) {
                            if (grp[_i] != settings_listen_target && *grp[_i] == btn) {
                                conflict = true;
                                break;
                            }
                        }
                        if (conflict) {
                            strncpy(settings_toast_msg, "Already bound!", sizeof(settings_toast_msg) - 1);
                            settings_save_toast = SDL_GetTicks() + 1800;
                            settings_toast_tw   = 0;
                            if (font_menu) TTF_SizeText(font_menu, settings_toast_msg, &settings_toast_tw, NULL);
                        } else if (settings_listen_target) {
                            *settings_listen_target = btn;
                            settings_dirty = true;
                            vtree_log("Keybind -> %s\n", SDL_GameControllerGetStringForButton(btn));
                        }
                        settings_listening     = false;
                        settings_listen_target = NULL;
                    } else {
                        int tab_n; tab_defs(&tab_n);
                        if (btn == cfg.k_back) {
                            settings_try_close();
                        } else if (btn == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                            settings_tab_indices[settings_tab] = settings_index;
                            settings_tab = (settings_tab - 1 + SETTINGS_TAB_COUNT) % SETTINGS_TAB_COUNT;
                            tab_defs(&tab_n);
                            settings_index = SDL_clamp(settings_tab_indices[settings_tab], 0, tab_n - 1);
                        } else if (btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                            settings_tab_indices[settings_tab] = settings_index;
                            settings_tab = (settings_tab + 1) % SETTINGS_TAB_COUNT;
                            tab_defs(&tab_n);
                            settings_index = SDL_clamp(settings_tab_indices[settings_tab], 0, tab_n - 1);
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                            settings_index = (settings_index - 1 + tab_n) % tab_n;
                            dpad_up_held = true; next_up_tick = now + REPEAT_DELAY;
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                            settings_index = (settings_index + 1) % tab_n;
                            dpad_down_held = true; next_down_tick = now + REPEAT_DELAY;
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
                            settings_adjust(-1);
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                            settings_adjust(+1);
                        } else if (btn == cfg.k_confirm) {
                            settings_confirm();
                        }
                    }

                // ── OSK ───────────────────────────────────────────────────────
                // L1 / R1  → caret left / right (always, dedicated held flags).
                // D-pad    → grid navigation when kb_visible; caret when hidden.
                // All face/menu buttons use cfg.osk_k_* (fully rebindable).
                } else if (current_mode == MODE_OSK) {

                    if (btn == cfg.osk_k_cancel) {
                        if (osk_purpose == OSK_FOR_TEXT_EDIT) {
                            viewer_close();
                            current_mode = MODE_VIEW_TEXT;
                        } else if (osk_purpose == OSK_FOR_SETTINGS_PATH) {
                            osk_path_target = NULL;
                            current_mode    = MODE_SETTINGS;
                        } else {
                            current_mode = MODE_EXPLORER;
                        }
                        osk_purpose = OSK_FOR_RENAME;

                    } else if (btn == cfg.osk_k_type && !osk.kb_visible) {
                        osk_confirm();

                    } else if (btn == cfg.osk_k_shift) {
                        osk_cycle_layer();

                    } else if (btn == cfg.osk_k_ins) {
                        osk.insert_mode = !osk.insert_mode;

                    } else if (btn == cfg.osk_k_toggle) {
                        osk.kb_visible = !osk.kb_visible;

                    } else if (btn == cfg.osk_k_type) {
                        osk_press();

                    } else if (btn == cfg.osk_k_bksp) {
                        osk_backspace();
                        osk_bksp_held = true; next_bksp_tick = now + REPEAT_DELAY;

                    // L1 / R1 — dedicated text-caret movement, always active in OSK.
                    // Using their own held flags so they never alias D-pad repeats.
                    } else if (btn == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                        osk_cursor_left();
                        osk_cur_l_held = true; next_cur_l_tick = now + REPEAT_DELAY;

                    } else if (btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                        osk_cursor_right();
                        osk_cur_r_held = true; next_cur_r_tick = now + REPEAT_DELAY;

                    // D-pad: navigate grid when visible, move caret when hidden
                    } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                        if (osk.kb_visible) { osk_move(-1, 0); dpad_up_held = true; next_up_tick = now + REPEAT_DELAY; }
                    } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                        if (osk.kb_visible) { osk_move(+1, 0); dpad_down_held = true; next_down_tick = now + REPEAT_DELAY; }
                    } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
                        if (osk.kb_visible) { osk_move(0, -1); dpad_left_held = true; next_left_tick = now + REPEAT_DELAY; }
                        else { osk_cursor_left(); dpad_left_held = true; next_left_tick = now + REPEAT_DELAY; }
                    } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                        if (osk.kb_visible) { osk_move(0, +1); dpad_right_held = true; next_right_tick = now + REPEAT_DELAY; }
                        else { osk_cursor_right(); dpad_right_held = true; next_right_tick = now + REPEAT_DELAY; }
                    }

                // ── OPEN-AS CHOOSER ───────────────────────────────────────────
                } else if (current_mode == MODE_VIEW_CHOOSE) {
                    if (btn == cfg.k_back || btn == cfg.k_menu) {
                        current_mode = MODE_EXPLORER;
                    } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                        choose_selection = (choose_selection - 1 + choose_count) % choose_count;
                    } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                        choose_selection = (choose_selection + 1) % choose_count;
                    } else if (btn == cfg.k_confirm) {
                        int act = choose_actions[choose_selection];
                        if      (act == ACT_CANCEL) { current_mode = MODE_EXPLORER; }
                        else if (act == ACT_TEXT)   { viewer_open(choose_path); }
                        else if (act == ACT_HEX)    { hexview_open(choose_path); }
                        else if (act == ACT_IMG)    { imgview_open(choose_path); }
                        else if (act == ACT_INFO) {
                            current_mode = MODE_EXPLORER;
                            if (choose_marked == 1) {
                                // single marked item — show individual info
                                AppState *cs = &panes[choose_pane];
                                for (int i = 0; i < cs->file_count; i++) {
                                    if (cs->files[i].marked) {
                                        char fp[MAX_PATH];
                                        join_path(fp, cs->current_path, cs->files[i].name);
                                        show_fileinfo(fp); break;
                                    }
                                }
                            } else if (choose_marked > 1) {
                                show_fileinfo_multi();
                            } else {
                                show_fileinfo(choose_path);
                            }
                        }
                        else if (act == ACT_EXEC)   { vtree_exec_script(choose_path); }
                    }

                // ── TEXT VIEWER / EDITOR ──────────────────────────────────────
                } else if (current_mode == MODE_VIEW_TEXT) {
                    viewer_handle_button(btn, NULL, NULL, NULL, NULL, now);
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP)    { dpad_up_held    = true; next_up_tick    = now + REPEAT_DELAY; }
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  { dpad_down_held  = true; next_down_tick  = now + REPEAT_DELAY; }
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  { dpad_left_held  = true; next_left_tick  = now + REPEAT_DELAY; }
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) { dpad_right_held = true; next_right_tick = now + REPEAT_DELAY; }
                    if (viewer_osk_is_pending()) {
                        int li = viewer_osk_line_index();
                        osk_enter_tv(viewer_get_line(li));
                    }

                // ── HEX VIEWER / EDITOR ───────────────────────────────────────
                } else if (current_mode == MODE_VIEW_HEX) {
                    hexview_handle_button(btn, NULL, NULL, NULL, NULL, now);
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP)    { dpad_up_held    = true; next_up_tick    = now + REPEAT_DELAY; }
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  { dpad_down_held  = true; next_down_tick  = now + REPEAT_DELAY; }
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  { dpad_left_held  = true; next_left_tick  = now + REPEAT_DELAY; }
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) { dpad_right_held = true; next_right_tick = now + REPEAT_DELAY; }

                // ── IMAGE VIEWER ──────────────────────────────────────────────
                } else if (current_mode == MODE_VIEW_IMAGE) {
                    imgview_handle_button(btn, now);
                    // pan repeat — all four directions
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP)    { dpad_up_held    = true; next_up_tick    = now + REPEAT_DELAY; }
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  { dpad_down_held  = true; next_down_tick  = now + REPEAT_DELAY; }
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  { dpad_left_held  = true; next_left_tick  = now + REPEAT_DELAY; }
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) { dpad_right_held = true; next_right_tick = now + REPEAT_DELAY; }
                }
            }
        }

        // ── D-pad + shoulder + backspace auto-repeat ─────────────────────────
        const Uint32 VIEWER_RATE = 150;
        if (dpad_up_held && now >= next_up_tick) {
            if      (current_mode == MODE_EXPLORER)     { AppState *s = &panes[active_pane]; if (s->selected_index > 0) s->selected_index--; else if (s->file_count > 0) s->selected_index = s->file_count - 1; }
            else if (current_mode == MODE_CONTEXT_MENU) {
                if (menu_in_files) {
                    filemenu_sel = (filemenu_sel - 1 + FILEMENU_MAX) % FILEMENU_MAX;
                    if (filemenu_sel == FILEMENU_PASTE && clip.op == OP_NONE)
                        filemenu_sel = (filemenu_sel - 1 + FILEMENU_MAX) % FILEMENU_MAX;
                    delete_confirm_active = false;
                } else {
                    menu_selection = (menu_selection - 1 + TOPMENU_MAX) % TOPMENU_MAX;
                }
            }
            else if (current_mode == MODE_SETTINGS)     { int _n; tab_defs(&_n); settings_index = (settings_index - 1 + _n) % _n; }
            else if (current_mode == MODE_OSK && osk.kb_visible) { osk_move(-1, 0); }
            else if (current_mode == MODE_VIEW_TEXT)    { viewer_handle_button(SDL_CONTROLLER_BUTTON_DPAD_UP,   NULL, NULL, NULL, NULL, now); }
            else if (current_mode == MODE_VIEW_HEX)     { hexview_handle_button(SDL_CONTROLLER_BUTTON_DPAD_UP,  NULL, NULL, NULL, NULL, now); }
            else if (current_mode == MODE_VIEW_IMAGE)   { imgview_handle_button(SDL_CONTROLLER_BUTTON_DPAD_UP,  now); }
            next_up_tick = now + ((current_mode == MODE_VIEW_TEXT || current_mode == MODE_VIEW_HEX || current_mode == MODE_VIEW_IMAGE) ? VIEWER_RATE : REPEAT_RATE);
        }
        if (dpad_down_held && now >= next_down_tick) {
            if      (current_mode == MODE_EXPLORER)     { AppState *s = &panes[active_pane]; if (s->selected_index < s->file_count - 1) s->selected_index++; else s->selected_index = 0; }
            else if (current_mode == MODE_CONTEXT_MENU) {
                if (menu_in_files) {
                    filemenu_sel = (filemenu_sel + 1) % FILEMENU_MAX;
                    if (filemenu_sel == FILEMENU_PASTE && clip.op == OP_NONE)
                        filemenu_sel = (filemenu_sel + 1) % FILEMENU_MAX;
                    delete_confirm_active = false;
                } else {
                    menu_selection = (menu_selection + 1) % TOPMENU_MAX;
                }
            }
            else if (current_mode == MODE_SETTINGS)     { int _n; tab_defs(&_n); settings_index = (settings_index + 1) % _n; }
            else if (current_mode == MODE_OSK && osk.kb_visible) { osk_move(+1, 0); }
            else if (current_mode == MODE_VIEW_TEXT)    { viewer_handle_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN,  NULL, NULL, NULL, NULL, now); }
            else if (current_mode == MODE_VIEW_HEX)     { hexview_handle_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN, NULL, NULL, NULL, NULL, now); }
            else if (current_mode == MODE_VIEW_IMAGE)   { imgview_handle_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN, now); }
            next_down_tick = now + ((current_mode == MODE_VIEW_TEXT || current_mode == MODE_VIEW_HEX || current_mode == MODE_VIEW_IMAGE) ? VIEWER_RATE : REPEAT_RATE);
        }
        if (dpad_left_held && now >= next_left_tick) {
            if      (current_mode == MODE_EXPLORER && !cfg.single_pane) active_pane = 0;
            else if (current_mode == MODE_OSK && osk.kb_visible)  osk_move(0, -1);
            else if (current_mode == MODE_OSK && !osk.kb_visible) osk_cursor_left();
            else if (current_mode == MODE_VIEW_TEXT)   viewer_handle_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT,  NULL, NULL, NULL, NULL, now);
            else if (current_mode == MODE_VIEW_HEX)    hexview_handle_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT, NULL, NULL, NULL, NULL, now);
            else if (current_mode == MODE_VIEW_IMAGE)  imgview_handle_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT, now);
            next_left_tick = now + (current_mode == MODE_VIEW_TEXT || current_mode == MODE_VIEW_HEX || current_mode == MODE_VIEW_IMAGE ? VIEWER_RATE : REPEAT_RATE);
        }
        if (dpad_right_held && now >= next_right_tick) {
            if      (current_mode == MODE_EXPLORER && !cfg.single_pane) active_pane = 1;
            else if (current_mode == MODE_OSK && osk.kb_visible)  osk_move(0, +1);
            else if (current_mode == MODE_OSK && !osk.kb_visible) osk_cursor_right();
            else if (current_mode == MODE_VIEW_TEXT)   viewer_handle_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT,  NULL, NULL, NULL, NULL, now);
            else if (current_mode == MODE_VIEW_HEX)    hexview_handle_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, NULL, NULL, NULL, NULL, now);
            else if (current_mode == MODE_VIEW_IMAGE)  imgview_handle_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, now);
            next_right_tick = now + (current_mode == MODE_VIEW_TEXT || current_mode == MODE_VIEW_HEX || current_mode == MODE_VIEW_IMAGE ? VIEWER_RATE : REPEAT_RATE);
        }
        if (osk_bksp_held && now >= next_bksp_tick) {
            if (current_mode == MODE_OSK) osk_backspace();
            next_bksp_tick = now + REPEAT_RATE;
        }
        // OSK caret auto-repeat (L1 / R1) — bounded and independent of dpad
        if (osk_cur_l_held && now >= next_cur_l_tick) {
            if (current_mode == MODE_OSK) osk_cursor_left();
            next_cur_l_tick = now + REPEAT_RATE;
        }
        if (osk_cur_r_held && now >= next_cur_r_tick) {
            if (current_mode == MODE_OSK) osk_cursor_right();
            next_cur_r_tick = now + REPEAT_RATE;
        }

        glyph_frame++;

        // ── RENDER ───────────────────────────────────────────────────────────
        if (current_mode == MODE_SETTINGS) {
            draw_settings(); present_frame(); continue;
        }
        if (current_mode == MODE_VIEW_TEXT) {
            viewer_draw(); present_frame(); continue;
        }
        if (current_mode == MODE_VIEW_HEX) {
            hexview_draw(); present_frame(); continue;
        }
        if (current_mode == MODE_VIEW_IMAGE) {
            imgview_draw(); present_frame(); continue;
        }
        if (current_mode == MODE_SNAKE) {
            snake_tick(now); snake_draw(); present_frame(); continue;
        }

        SDL_SetRenderDrawColor(renderer, cfg.theme.bg.r, cfg.theme.bg.g, cfg.theme.bg.b, 255);
        SDL_RenderClear(renderer);

        int head_h = cfg.font_size_header + 12;
        int foot_h = cfg.font_size_footer + 16;
        // Icon size: native 32px; row must be tall enough to fit without scaling.
        // We also want a small amount of padding above and below the icon.
        const int ICO_SIZE = 32;
        const int ICO_PAD  = 3;   // px above and below icon within row
        int item_h = (font_list ? TTF_FontHeight(font_list) : cfg.font_size_list) + 6;
        if (item_h < ICO_SIZE + ICO_PAD * 2)
            item_h = ICO_SIZE + ICO_PAD * 2;
        int pane_w = cfg.single_pane ? cfg.screen_w : cfg.screen_w / 2;
        int p_begin = cfg.single_pane ? active_pane : 0;
        int p_end   = cfg.single_pane ? active_pane + 1 : 2;

        for (int p = p_begin; p < p_end; p++) {
            int x = cfg.single_pane ? 0 : p * pane_w; AppState *s = &panes[p];
            SDL_SetRenderDrawColor(renderer, cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
            SDL_Rect hr = {x, 0, pane_w - 1, head_h}; SDL_RenderFillRect(renderer, &hr);
            SDL_Rect pane_clip = {x, 0, pane_w - 1, cfg.screen_h};
            SDL_RenderSetClipRect(renderer, &pane_clip);
            draw_txt(font_header, s->current_path, x + 5, 5, cfg.theme.text);
            int max_v = (cfg.screen_h - head_h - foot_h) / item_h;
            if (s->selected_index >= s->scroll_offset + max_v) s->scroll_offset = s->selected_index - max_v + 1;
            if (s->selected_index <  s->scroll_offset)         s->scroll_offset = s->selected_index;
            for (int i = s->scroll_offset; i < s->file_count && i < s->scroll_offset + max_v; i++) {
                int iy = head_h + (i - s->scroll_offset) * item_h;
                if (i % 2 != 0) { SDL_SetRenderDrawColor(renderer, cfg.theme.alt_bg.r, cfg.theme.alt_bg.g, cfg.theme.alt_bg.b, 255); SDL_Rect ar={x,iy,pane_w-1,item_h}; SDL_RenderFillRect(renderer,&ar); }
                if (p == active_pane && i == s->selected_index) { SDL_SetRenderDrawColor(renderer, cfg.theme.highlight_bg.r, cfg.theme.highlight_bg.g, cfg.theme.highlight_bg.b, 255); SDL_Rect sr={x,iy,pane_w-1,item_h}; SDL_RenderFillRect(renderer,&sr); }
                SDL_Texture *icn = get_file_icon(&s->files[i]);
                bool is_selected = (p == active_pane && i == s->selected_index);
                bool in_clip = (clip.op != OP_NONE) && file_in_clipboard(p, i);
                bool is_cut  = in_clip && (clip.op == OP_CUT);
                if (icn) {
                    SDL_Rect ir={x+5, iy+ICO_PAD, ICO_SIZE, ICO_SIZE};
                    SDL_SetTextureAlphaMod(icn, is_cut ? 100 : 255);
                    SDL_RenderCopy(renderer,icn,NULL,&ir);
                    SDL_SetTextureAlphaMod(icn, 255);
                }
                SDL_Color name_col;
                if (is_cut)
                    name_col = cfg.theme.text_disabled;
                else if (in_clip)  // copied
                    name_col = is_selected ? cfg.theme.highlight_text : cfg.theme.link;
                else
                    name_col = s->files[i].marked ? cfg.theme.marked : (is_selected ? cfg.theme.highlight_text : (s->files[i].is_link ? cfg.theme.link : cfg.theme.text));
                draw_txt(font_list, s->files[i].name, x+ICO_SIZE+10, iy+(item_h-cfg.font_size_list)/2, name_col);
            }
            SDL_RenderSetClipRect(renderer, NULL);
        }

        // Footer
        SDL_SetRenderDrawColor(renderer, cfg.theme.header_bg.r, cfg.theme.header_bg.g, cfg.theme.header_bg.b, 255);
        SDL_Rect fr = {0, cfg.screen_h - foot_h, cfg.screen_w, foot_h}; SDL_RenderFillRect(renderer, &fr);
        char f_text[256];
        if (panes[active_pane].file_count > 0) {
            FileEntry *sel = &panes[active_pane].files[panes[active_pane].selected_index];
            char sz[16] = "DIR"; if (!sel->is_dir) format_size(sel->size, sz);
            snprintf(f_text, 256, "P%d [%d/%d] %s | %s", active_pane+1, panes[active_pane].selected_index+1, panes[active_pane].file_count, sel->name, sz);
        } else { snprintf(f_text, 256, "P%d Empty", active_pane+1); }
        draw_txt(font_footer, f_text, 12, cfg.screen_h - foot_h + 6, cfg.theme.text);
        // Clipboard badge — icon + count, floating above bottom-right corner
        if (clip.op != OP_NONE) {
            SDL_Texture *clip_icon = (clip.op == OP_CUT) ? tex_cut : tex_copy;
            const int badge  = 52;   // badge square side in px
            const int ico    = 26;   // icon size within badge
            const int pad    = 5;    // inner padding from border
            const int margin = 8;    // gap from screen right / above footer
            int bx = cfg.screen_w - badge - margin;
            int by = cfg.screen_h - foot_h - badge - margin;

            // Background square
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_Rect bbg = {bx, by, badge, badge};
            SDL_SetRenderDrawColor(renderer, cfg.theme.menu_bg.r, cfg.theme.menu_bg.g,
                                   cfg.theme.menu_bg.b, cfg.theme.menu_bg.a);
            SDL_RenderFillRect(renderer, &bbg);
            SDL_SetRenderDrawColor(renderer, cfg.theme.menu_border.r, cfg.theme.menu_border.g,
                                   cfg.theme.menu_border.b, 255);
            SDL_RenderDrawRect(renderer, &bbg);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

            // Icon: top-left with padding
            if (clip_icon) {
                SDL_Rect ir = {bx + pad, by + pad, ico, ico};
                SDL_SetTextureAlphaMod(clip_icon, 255);
                SDL_RenderCopy(renderer, clip_icon, NULL, &ir);
            }

            // Count: bottom-right with padding
            char cnt[8];
            snprintf(cnt, sizeof(cnt), "%d", clip.count);
            int tw = 0, th = 0;
            if (font_footer) TTF_SizeUTF8(font_footer, cnt, &tw, &th);
            draw_txt(font_footer, cnt,
                     bx + badge - tw - pad,
                     by + badge - th - pad,
                     cfg.theme.marked);
        }

        // Context menu overlay
        if (current_mode == MODE_CONTEXT_MENU) {
            int pi=6, isz=32, spc=(cfg.font_size_menu>isz?cfg.font_size_menu:isz)+pi;
            bool in_sub = menu_in_files;
            int  item_count = in_sub ? FILEMENU_MAX : TOPMENU_MAX;
            int mw=270, mh=item_count*spc+20, mx=(cfg.screen_w-mw)/2, my=(cfg.screen_h-mh)/2;
            SDL_Rect mbg={mx,my,mw,mh};
            SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer,cfg.theme.menu_bg.r,cfg.theme.menu_bg.g,cfg.theme.menu_bg.b,cfg.theme.menu_bg.a);
            SDL_RenderFillRect(renderer,&mbg);
            SDL_SetRenderDrawColor(renderer,cfg.theme.menu_border.r,cfg.theme.menu_border.g,cfg.theme.menu_border.b,cfg.theme.menu_border.a);
            SDL_RenderDrawRect(renderer,&mbg);
            SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);

            if (!in_sub) {
                // ── Top-level menu ─────────────────────────────────────────
                SDL_Texture *top_icons[TOPMENU_MAX] = { tex_folder, tex_settings, tex_about, tex_exit };
                for (int i = 0; i < TOPMENU_MAX; i++) {
                    bool sel = (i == menu_selection);
                    SDL_Color c = sel ? cfg.theme.marked : cfg.theme.text;
                    int iy = my + 10 + i * spc;
                    if (top_icons[i]) { SDL_Rect ir={mx+15,iy+(spc-isz)/2,isz,isz}; SDL_SetTextureAlphaMod(top_icons[i],255); SDL_RenderCopy(renderer,top_icons[i],NULL,&ir); }
                    draw_txt(font_menu, topmenu_items[i], mx+60, iy+(spc-cfg.font_size_menu)/2, c);
                    // Arrow indicator for Files entry
                    if (i == TOPMENU_FILES && tex_enterfol) {
                        SDL_Rect ar = { mx+mw-isz-15, iy+(spc-isz)/2, isz, isz };
                        SDL_SetTextureAlphaMod(tex_enterfol, 255);
                        SDL_RenderCopy(renderer, tex_enterfol, NULL, &ar);
                    }
                }
            } else {
                // ── File ops submenu ────────────────────────────────────────
                SDL_Texture *sub_icons[FILEMENU_MAX] = {
                    tex_copy, tex_cut, tex_paste, tex_symlink, tex_rename,
                    tex_delete, tex_newfile, tex_newfolder, tex_dirup
                };
                bool _any_m = false;
                AppState *rs = &panes[active_pane];
                for (int ri = 0; ri < rs->file_count; ri++) if (rs->files[ri].marked) { _any_m = true; break; }
                bool _dotdot = (!_any_m && rs->file_count > 0 &&
                                strcmp(rs->files[rs->selected_index].name, "..") == 0);
                for (int i = 0; i < FILEMENU_MAX; i++) {
                    bool dis = (i == FILEMENU_PASTE   && clip.op == OP_NONE) ||
                               (i == FILEMENU_SYMLINK && (clip.op != OP_COPY || clip.count == 0 ||
                                   (!fs_supports_symlinks(panes[0].current_path) &&
                                    !fs_supports_symlinks(panes[1].current_path)))) ||
                               (_dotdot && (i == FILEMENU_COPY || i == FILEMENU_CUT ||
                                            i == FILEMENU_RENAME || i == FILEMENU_DELETE));
                    bool sel = (i == filemenu_sel);
                    SDL_Color c = dis ? cfg.theme.text_disabled : (sel ? cfg.theme.marked : cfg.theme.text);
                    const char *lbl = filemenu_items[i];
                    if (i == FILEMENU_DELETE && delete_confirm_active) lbl = "CONFIRM DELETE?";
                    int iy = my + 10 + i * spc;
                    if (sub_icons[i]) { SDL_Rect ir={mx+15,iy+(spc-isz)/2,isz,isz}; SDL_SetTextureAlphaMod(sub_icons[i],dis?80:255); SDL_RenderCopy(renderer,sub_icons[i],NULL,&ir); }
                    draw_txt(font_menu, lbl, mx+60, iy+(spc-cfg.font_size_menu)/2, c);
                }
            }

            // About modal on top of context menu
            if (about_active) draw_about_modal();
        }

        // OSK overlay (on top of the explorer background)
        if (current_mode == MODE_OSK) draw_osk();

        // Paste destination picker overlay
        if (current_mode == MODE_CONTEXT_MENU && paste_dest_active)
            draw_paste_dest();

        // Paste conflict overlay (on top of context menu)
        if (current_mode == MODE_CONTEXT_MENU && paste_conflict_active)
            draw_paste_conflict();

        // Action chooser overlay
        if (current_mode == MODE_VIEW_CHOOSE) draw_open_chooser();

        // Execute error modal overlay
        if (current_mode == MODE_EXPLORER && exec_error_active) draw_exec_error_modal();

        // File info modal overlay
        if (current_mode == MODE_EXPLORER && fileinfo_active) draw_fileinfo_modal();

        present_frame();
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    hexview_close();
    imgview_close();
    if (pad) SDL_GameControllerClose(pad);
    TTF_CloseFont(font_list); TTF_CloseFont(font_header); TTF_CloseFont(font_footer); TTF_CloseFont(font_menu); TTF_CloseFont(font_hex);
    SDL_DestroyTexture(tex_file); SDL_DestroyTexture(tex_folder);
    SDL_DestroyTexture(tex_img);  SDL_DestroyTexture(tex_txt); SDL_DestroyTexture(tex_dirup);
    SDL_DestroyTexture(tex_copy); SDL_DestroyTexture(tex_cut); SDL_DestroyTexture(tex_paste); SDL_DestroyTexture(tex_symlink);
    SDL_DestroyTexture(tex_rename); SDL_DestroyTexture(tex_delete);
    SDL_DestroyTexture(tex_settings); SDL_DestroyTexture(tex_exit);
    SDL_DestroyTexture(tex_newfile); SDL_DestroyTexture(tex_newfolder); SDL_DestroyTexture(tex_about);
    SDL_DestroyTexture(tex_enterfol);
    SDL_DestroyTexture(tex_viewer); SDL_DestroyTexture(tex_hexview);
    SDL_DestroyTexture(tex_imgview); SDL_DestroyTexture(tex_fileinfo); SDL_DestroyTexture(tex_exec);
    destroy_glyph_cache();
    if (render_target) SDL_DestroyTexture(render_target);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
    IMG_Quit(); TTF_Quit(); SDL_Quit();
    vtree_log("Clean exit.\n");
    if (debug_log_file) { fclose(debug_log_file); debug_log_file = NULL; }
    return 0;
}